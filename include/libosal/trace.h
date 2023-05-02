/**
 * \file trace.h
 *
 * \author Robert Burger <robert.burger@dlr.de>
 *
 * \date 02 May 2023
 *
 * \brief OSAL trace header.
 *
 * OSAL trace include header.
 */

/*
 * This file is part of libosal.
 *
 * libosal is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libosal is distributed in the hope that 
 * it will be useful, but WITHOUT ANY WARRANTY; without even the implied 
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libosal. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBOSAL_TRACE__H
#define LIBOSAL_TRACE__H

#include <libosal/config.h>
#include <libosal/types.h>
#include <libosal/osal.h>
#include <libosal/trace.h>
#include <libosal/task.h>

typedef struct osal_trace {
    osal_uint32_t cnt;
    osal_uint32_t act_buf;
    osal_uint32_t pos;
    osal_binary_semaphore_t sync_sem;
    osal_task_t task;
    osal_uint64_t *time_in_ns[2];
    osal_uint64_t *tmp;
} osal_trace_t;

#ifdef __cplusplus
extern "C" {
#endif

//! \brief Allocate trace struct.
/*!
 * \param[out]  trace   Pointer to trace* where allocated trace struct is returned.
 * \param[in]   cnt     Number of samples to allocate.
 *                      the defaults of the underlying task will be used.
 *
 * \return OK or ERROR_CODE.
 */
int osal_trace_alloc(osal_trace_t **trace, osal_uint32_t cnt);

//! \brief Free trace struct.
/*!
 * \param[in]   trace   Pointer to trace struct to free.
 *
 * \return N/A
 */
void osal_trace_free(osal_trace_t *trace);

//! \brief Trace time.
/*!
 * \param[in]   trace   Pointer to trace struct.
 *
 * \return N/A
 */
void osal_trace_point(osal_trace_t *trace);

//! \brief Run trace task.
/*!
 * \param[in]   trace   Pointer to trace struct.
 *
 * \return N/A
 */
void osal_trace_run(osal_trace_t *trace);

//! \brief Stop trace task.
/*!
 * \param[in]   trace   Pointer to trace struct.
 *
 * \return N/A
 */
void osal_trace_stop(osal_trace_t *trace);

//! \brief Analyze trace and return average and jitters.
/*!
 * \param[in]   trace   Pointer to trace struct.
 * \param[out]  avg     Return average time interval.
 * \param[out]  avg_jit Return average jitter (std-dev).
 * \param[out]  max_jit Return maximum jitter.
 *
 * \return N/A
 */
void osal_trace_analyze(osal_trace_t *trace, osal_uint64_t *avg, osal_uint64_t *avg_jit, osal_uint64_t *max_jit);

#ifdef __cplusplus
};
#endif

#endif /* LIBOSAL_TRACE__H */

