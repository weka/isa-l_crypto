/**********************************************************************
  Copyright(c) 2022 Arm Corporation All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Arm Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********************************************************************/

#include <stdint.h>
#include <string.h>
#include "sm3_mb.h"
#include "sm3_mb_internal.h"
#include "memcpy_inline.h"
#include "endian_helper.h"
void
sm3_mb_mgr_init_sve2(ISAL_SM3_MB_JOB_MGR *state);
ISAL_SM3_JOB *
sm3_mb_mgr_submit_sve2(ISAL_SM3_MB_JOB_MGR *state, ISAL_SM3_JOB *job);
ISAL_SM3_JOB *
sm3_mb_mgr_flush_sve2(ISAL_SM3_MB_JOB_MGR *state);
static inline void
hash_init_digest(ISAL_SM3_WORD_T *digest);
static inline uint32_t
hash_pad(uint8_t padblock[ISAL_SM3_BLOCK_SIZE * 2], uint64_t total_len);
static ISAL_SM3_HASH_CTX *
sm3_ctx_mgr_resubmit(ISAL_SM3_HASH_CTX_MGR *mgr, ISAL_SM3_HASH_CTX *ctx);

void
sm3_ctx_mgr_init_sve2(ISAL_SM3_HASH_CTX_MGR *mgr)
{
        sm3_mb_mgr_init_sve2(&mgr->mgr);
}

ISAL_SM3_HASH_CTX *
sm3_ctx_mgr_submit_sve2(ISAL_SM3_HASH_CTX_MGR *mgr, ISAL_SM3_HASH_CTX *ctx, const void *buffer,
                        uint32_t len, ISAL_HASH_CTX_FLAG flags)
{
        if (flags & (~ISAL_HASH_ENTIRE)) {
                // User should not pass anything other than FIRST, UPDATE, or LAST
                ctx->error = ISAL_HASH_CTX_ERROR_INVALID_FLAGS;
                return ctx;
        }

        if (ctx->status & ISAL_HASH_CTX_STS_PROCESSING) {
                // Cannot submit to a currently processing job.
                ctx->error = ISAL_HASH_CTX_ERROR_ALREADY_PROCESSING;
                return ctx;
        }

        if ((ctx->status & ISAL_HASH_CTX_STS_COMPLETE) && !(flags & ISAL_HASH_FIRST)) {
                // Cannot update a finished job.
                ctx->error = ISAL_HASH_CTX_ERROR_ALREADY_COMPLETED;
                return ctx;
        }

        if (flags & ISAL_HASH_FIRST) {
                // Init digest
                hash_init_digest(ctx->job.result_digest);

                // Reset byte counter
                ctx->total_length = 0;

                // Clear extra blocks
                ctx->partial_block_buffer_length = 0;
        }
        // If we made it here, there were no errors during this call to submit
        ctx->error = ISAL_HASH_CTX_ERROR_NONE;

        // Store buffer ptr info from user
        ctx->incoming_buffer = buffer;
        ctx->incoming_buffer_length = len;

        // Store the user's request flags and mark this ctx as currently being processed.
        ctx->status = (flags & ISAL_HASH_LAST) ? (ISAL_HASH_CTX_STS) (ISAL_HASH_CTX_STS_PROCESSING |
                                                                      ISAL_HASH_CTX_STS_LAST)
                                               : ISAL_HASH_CTX_STS_PROCESSING;

        // Advance byte counter
        ctx->total_length += len;

        // If there is anything currently buffered in the extra blocks, append to it until it
        // contains a whole block. Or if the user's buffer contains less than a whole block, append
        // as much as possible to the extra block.
        if ((ctx->partial_block_buffer_length) | (len < ISAL_SM3_BLOCK_SIZE)) {
                // Compute how many bytes to copy from user buffer into extra block
                uint32_t copy_len = ISAL_SM3_BLOCK_SIZE - ctx->partial_block_buffer_length;
                if (len < copy_len)
                        copy_len = len;

                if (copy_len) {
                        // Copy and update relevant pointers and counters
                        memcpy_fixedlen(
                                &ctx->partial_block_buffer[ctx->partial_block_buffer_length],
                                buffer, copy_len);

                        ctx->partial_block_buffer_length += copy_len;
                        ctx->incoming_buffer = (const void *) ((const char *) buffer + copy_len);
                        ctx->incoming_buffer_length = len - copy_len;
                }
                // The extra block should never contain more than 1 block here
                assert(ctx->partial_block_buffer_length <= ISAL_SM3_BLOCK_SIZE);

                // If the extra block buffer contains exactly 1 block, it can be hashed.
                if (ctx->partial_block_buffer_length >= ISAL_SM3_BLOCK_SIZE) {
                        ctx->partial_block_buffer_length = 0;

                        ctx->job.buffer = ctx->partial_block_buffer;
                        ctx->job.len = 1;

                        ctx = (ISAL_SM3_HASH_CTX *) sm3_mb_mgr_submit_sve2(&mgr->mgr, &ctx->job);
                }
        }

        return sm3_ctx_mgr_resubmit(mgr, ctx);
}

ISAL_SM3_HASH_CTX *
sm3_ctx_mgr_flush_sve2(ISAL_SM3_HASH_CTX_MGR *mgr)
{
        ISAL_SM3_HASH_CTX *ctx;

        while (1) {
                ctx = (ISAL_SM3_HASH_CTX *) sm3_mb_mgr_flush_sve2(&mgr->mgr);

                // If flush returned 0, there are no more jobs in flight.
                if (!ctx)
                        return NULL;

                // If flush returned a job, verify that it is safe to return to the user.
                // If it is not ready, resubmit the job to finish processing.
                ctx = sm3_ctx_mgr_resubmit(mgr, ctx);

                // If sm3_ctx_mgr_resubmit returned a job, it is ready to be returned.
                if (ctx)
                        return ctx;

                // Otherwise, all jobs currently being managed by the ISAL_SM3_HASH_CTX_MGR still
                // need processing. Loop.
        }
}

static ISAL_SM3_HASH_CTX *
sm3_ctx_mgr_resubmit(ISAL_SM3_HASH_CTX_MGR *mgr, ISAL_SM3_HASH_CTX *ctx)
{
        while (ctx) {

                if (ctx->status & ISAL_HASH_CTX_STS_COMPLETE) {
                        ctx->status = ISAL_HASH_CTX_STS_COMPLETE; // Clear PROCESSING bit
                        return ctx;
                }
                // If the extra blocks are empty, begin hashing what remains in the user's buffer.
                if (ctx->partial_block_buffer_length == 0 && ctx->incoming_buffer_length) {
                        const void *buffer = ctx->incoming_buffer;
                        uint32_t len = ctx->incoming_buffer_length;

                        // Only entire blocks can be hashed. Copy remainder to extra blocks buffer.
                        uint32_t copy_len = len & (ISAL_SM3_BLOCK_SIZE - 1);

                        if (copy_len) {
                                len -= copy_len;
                                memcpy_fixedlen(ctx->partial_block_buffer,
                                                ((const char *) buffer + len), copy_len);
                                ctx->partial_block_buffer_length = copy_len;
                        }

                        ctx->incoming_buffer_length = 0;

                        // len should be a multiple of the block size now
                        assert((len % ISAL_SM3_BLOCK_SIZE) == 0);

                        // Set len to the number of blocks to be hashed in the user's buffer
                        len >>= ISAL_SM3_LOG2_BLOCK_SIZE;

                        if (len) {
                                ctx->job.buffer = (uint8_t *) buffer;
                                ctx->job.len = len;
                                ctx = (ISAL_SM3_HASH_CTX *) sm3_mb_mgr_submit_sve2(&mgr->mgr,
                                                                                   &ctx->job);
                                continue;
                        }
                }
                // If the extra blocks are not empty, then we are either on the last block(s)
                // or we need more user input before continuing.
                if (ctx->status & ISAL_HASH_CTX_STS_LAST) {
                        uint8_t *buf = ctx->partial_block_buffer;
                        uint32_t n_extra_blocks = hash_pad(buf, ctx->total_length);

                        ctx->status = (ISAL_HASH_CTX_STS) (ISAL_HASH_CTX_STS_PROCESSING |
                                                           ISAL_HASH_CTX_STS_COMPLETE);
                        ctx->job.buffer = buf;
                        ctx->job.len = (uint32_t) n_extra_blocks;
                        ctx = (ISAL_SM3_HASH_CTX *) sm3_mb_mgr_submit_sve2(&mgr->mgr, &ctx->job);
                        continue;
                }

                if (ctx)
                        ctx->status = ISAL_HASH_CTX_STS_IDLE;
                return ctx;
        }

        return NULL;
}

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define cpu_to_be32(v)                                                                             \
        (((v & 0xff000000) >> 24) | ((v & 0xff0000) >> 8) | ((v & 0xff00) << 8) |                  \
         ((v & 0xff) << 24))
#else
#define cpu_to_be32(v)
#endif
static inline void
hash_init_digest(ISAL_SM3_WORD_T *digest)
{
        static const ISAL_SM3_WORD_T hash_initial_digest[ISAL_SM3_DIGEST_NWORDS] = {
                cpu_to_be32(0x7380166f), cpu_to_be32(0x4914b2b9), cpu_to_be32(0x172442d7),
                cpu_to_be32(0xda8a0600), cpu_to_be32(0xa96f30bc), cpu_to_be32(0x163138aa),
                cpu_to_be32(0xe38dee4d), cpu_to_be32(0xb0fb0e4e)
        };
        memcpy_fixedlen(digest, hash_initial_digest, sizeof(hash_initial_digest));
}

static inline uint32_t
hash_pad(uint8_t padblock[ISAL_SM3_BLOCK_SIZE * 2], uint64_t total_len)
{
        uint32_t i = (uint32_t) (total_len & (ISAL_SM3_BLOCK_SIZE - 1));

        memclr_fixedlen(&padblock[i], ISAL_SM3_BLOCK_SIZE);
        padblock[i] = 0x80;

        // Move i to the end of either 1st or 2nd extra block depending on length
        i += ((ISAL_SM3_BLOCK_SIZE - 1) & (0 - (total_len + ISAL_SM3_PADLENGTHFIELD_SIZE + 1))) +
             1 + ISAL_SM3_PADLENGTHFIELD_SIZE;

#if ISAL_SM3_PADLENGTHFIELD_SIZE == 16
        *((uint64_t *) &padblock[i - 16]) = 0;
#endif

        *((uint64_t *) &padblock[i - 8]) = to_be64((uint64_t) total_len << 3);

        return i >> ISAL_SM3_LOG2_BLOCK_SIZE; // Number of extra blocks to hash
}
