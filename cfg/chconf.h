/*
    ChibiOS - Copyright (C) 2006..2025 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/*
 * RT kernel configuration for CMSIS-DAP probe (dual-core SMP).
 */

#ifndef CHCONF_H
#define CHCONF_H

#define _CHIBIOS_RT_CONF_
#define _CHIBIOS_RT_CONF_VER_8_0_

/*===========================================================================*/
/* System settings                                                           */
/*===========================================================================*/

#define CH_CFG_SMP_MODE                     TRUE
#define CH_CFG_HARDENING_LEVEL              0

/*===========================================================================*/
/* System timers settings                                                    */
/*===========================================================================*/

#define CH_CFG_ST_RESOLUTION                32
#define CH_CFG_ST_FREQUENCY                 1000000
#define CH_CFG_ST_TIMEDELTA                 20
#define CH_CFG_INTERVALS_SIZE               32
#define CH_CFG_TIME_TYPES_SIZE              32

/*===========================================================================*/
/* Kernel parameters                                                         */
/*===========================================================================*/

#define CH_CFG_TIME_QUANTUM                 0
#define CH_CFG_NO_IDLE_THREAD               FALSE
#define CH_CFG_OPTIMIZE_SPEED               TRUE

/*===========================================================================*/
/* Subsystem options                                                         */
/*===========================================================================*/

#define CH_CFG_USE_TM                       FALSE
#define CH_CFG_USE_TIMESTAMP                FALSE
#define CH_CFG_USE_REGISTRY                 FALSE
#define CH_CFG_USE_WAITEXIT                 FALSE
#define CH_CFG_USE_SEMAPHORES               TRUE
#define CH_CFG_USE_SEMAPHORES_PRIORITY      FALSE
#define CH_CFG_USE_MUTEXES                  TRUE
#define CH_CFG_USE_MUTEXES_RECURSIVE        FALSE
#define CH_CFG_USE_CONDVARS                 FALSE
#define CH_CFG_USE_CONDVARS_TIMEOUT         FALSE
#define CH_CFG_USE_EVENTS                   TRUE
#define CH_CFG_USE_EVENTS_TIMEOUT           TRUE
#define CH_CFG_USE_MESSAGES                 FALSE
#define CH_CFG_USE_MESSAGES_PRIORITY        FALSE
#define CH_CFG_USE_DYNAMIC                  FALSE

/*===========================================================================*/
/* OSLIB options                                                             */
/*===========================================================================*/

#define CH_CFG_USE_MAILBOXES                TRUE
#define CH_CFG_USE_MEMCHECKS                FALSE
#define CH_CFG_USE_MEMCORE                  TRUE
#define CH_CFG_MEMCORE_SIZE                 0
#define CH_CFG_USE_HEAP                     FALSE
#define CH_CFG_USE_MEMPOOLS                 TRUE
#define CH_CFG_USE_OBJ_FIFOS                TRUE
#define CH_CFG_USE_PIPES                    FALSE
#define CH_CFG_USE_OBJ_CACHES              FALSE
#define CH_CFG_USE_DELEGATES                FALSE
#define CH_CFG_USE_JOBS                     FALSE

/*===========================================================================*/
/* Objects factory                                                           */
/*===========================================================================*/

#define CH_CFG_USE_FACTORY                  FALSE
#define CH_CFG_FACTORY_MAX_NAMES_LENGTH     8
#define CH_CFG_FACTORY_OBJECTS_REGISTRY     FALSE
#define CH_CFG_FACTORY_GENERIC_BUFFERS      FALSE
#define CH_CFG_FACTORY_SEMAPHORES           FALSE
#define CH_CFG_FACTORY_MAILBOXES            FALSE
#define CH_CFG_FACTORY_OBJ_FIFOS            FALSE
#define CH_CFG_FACTORY_PIPES                FALSE

/*===========================================================================*/
/* Debug options                                                             */
/*===========================================================================*/

#define CH_DBG_STATISTICS                   FALSE
#define CH_DBG_SYSTEM_STATE_CHECK           FALSE
#define CH_DBG_ENABLE_CHECKS                FALSE
#define CH_DBG_ENABLE_ASSERTS               FALSE
#define CH_DBG_TRACE_MASK                   CH_DBG_TRACE_MASK_DISABLED
#define CH_DBG_TRACE_BUFFER_SIZE            128
#ifndef CH_DBG_ENABLE_STACK_CHECK
#define CH_DBG_ENABLE_STACK_CHECK           FALSE
#endif
#define CH_DBG_FILL_THREADS                 FALSE
#define CH_DBG_THREADS_PROFILING            FALSE

/*===========================================================================*/
/* Kernel hooks                                                              */
/*===========================================================================*/

#define CH_CFG_SYSTEM_EXTRA_FIELDS
#define CH_CFG_SYSTEM_INIT_HOOK()
#define CH_CFG_OS_INSTANCE_EXTRA_FIELDS
#define CH_CFG_OS_INSTANCE_INIT_HOOK(oip)
#define CH_CFG_THREAD_EXTRA_FIELDS
#define CH_CFG_THREAD_INIT_HOOK(tp)
#define CH_CFG_THREAD_EXIT_HOOK(tp)
#define CH_CFG_CONTEXT_SWITCH_HOOK(ntp, otp)
#define CH_CFG_IRQ_PROLOGUE_HOOK()
#define CH_CFG_IRQ_EPILOGUE_HOOK()
#define CH_CFG_IDLE_ENTER_HOOK()
#define CH_CFG_IDLE_LEAVE_HOOK()
#define CH_CFG_IDLE_LOOP_HOOK()
#define CH_CFG_SYSTEM_TICK_HOOK()
#define CH_CFG_SYSTEM_HALT_HOOK(reason)
#define CH_CFG_TRACE_HOOK(tep)
#define CH_CFG_RUNTIME_FAULTS_HOOK(mask)
#define CH_CFG_SAFETY_CHECK_HOOK(l, f)      chSysHalt(f)

#endif /* CHCONF_H */
