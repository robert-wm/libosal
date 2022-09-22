/**
 * \file types.h
 *
 * \author Robert Burger <robert.burger@dlr.de>
 *
 * \date 07 Aug 2022
 *
 * \brief OSAL types header.
 *
 * OSAL types include header.
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

#ifndef LIBOSAL_TYPES__H
#define LIBOSAL_TYPES__H

#include <stdint.h>
#include <stddef.h>

typedef void        osal_void_t;

typedef int         osal_retval_t;

typedef char        osal_char_t;

typedef size_t      osal_size_t;

#ifdef LIBOSAL_BUILD_PIKEOS
typedef ssize_t     int64_t;
#else 
#include <unistd.h>
typedef ssize_t     osal_ssize_t;
#endif

typedef uint8_t     osal_uint8_t;
typedef uint16_t    osal_uint16_t;
typedef uint32_t    osal_uint32_t;
typedef uint64_t    osal_uint64_t;

typedef int8_t     osal_int8_t;
typedef int16_t    osal_int16_t;
typedef int32_t    osal_int32_t;
typedef int64_t    osal_int64_t;

#endif /* LIBOSAL_TYPES__H */

