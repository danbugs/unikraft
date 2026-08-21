/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * Hyperlight host-call (hcall) API.
 *
 * Allows the guest to call functions registered on the host side.
 * Communication uses the PEB's shared I/O stacks with Hyperlight's
 * FlatBuffer-based protocol:
 *
 *   1. Guest encodes a FunctionCall FlatBuffer and pushes it onto
 *      the PEB output stack.
 *   2. Guest writes to port 101 (CallFunction) → VM exit.
 *   3. Host reads the output stack, dispatches the call, and writes
 *      the result (FunctionCallResult FlatBuffer) to the input stack.
 *   4. Guest resumes and pops the result from the input stack.
 */

#ifndef __HYPERLIGHT_HCALL_H__
#define __HYPERLIGHT_HCALL_H__

#include <uk/arch/types.h>
#include <hyperlight-x86/peb.h>

/**
 * Initialise the hcall subsystem with a copy of the PEB.
 *
 * Must be called before any hl_call_host_* function.  The PEB's
 * input_stack and output_stack pointers are cached internally.
 */
void hl_hcall_init(const struct hyperlight_peb *peb);

/**
 * Check whether the hcall subsystem has been initialised.
 */
int hl_hcall_ready(void);

/**
 * Call the GetCmdLine host function to retrieve the command line.
 *
 * @param out_buf   Buffer to receive the null-terminated string
 * @param buf_sz    Size of out_buf
 * @return          Length of the returned string (excluding NUL),
 *                  or -1 on error.
 */
int hl_call_get_cmdline(char *out_buf, __sz buf_sz);

/**
 * Call GetPagingBudget() to query how many bytes the guest should
 * allocate from scratch for the paging frame allocator.
 *
 * @return  Number of bytes (page-aligned), or 0 if the host function
 *          is not registered (caller should compute a dynamic default).
 */
__u64 hl_call_get_paging_budget(void);

/**
 * Call HostPrint — send a string to the host for printing.
 *
 * This calls the "HostPrint" host function with one string parameter.
 * Used by the console driver as an alternative to DebugPrint (port 103).
 * DebugPrint does one VM exit per byte; HostPrint does one VM exit for
 * the entire string.
 *
 * @param msg   String to print (not necessarily NUL-terminated)
 * @param len   Length of the string
 * @return      0 on success, -1 on error
 */
int hl_call_host_print(const char *msg, __sz len);

/*
 * FlatBuffer ReturnType constants (from function_types.fbs).
 * Used as expected_return_type in FunctionCall.
 */
#define HL_RT_INT	0
#define HL_RT_UINT	1
#define HL_RT_LONG	2
#define HL_RT_ULONG	3
#define HL_RT_FLOAT	4
#define HL_RT_DOUBLE	5
#define HL_RT_STRING	6
#define HL_RT_BOOL	7
#define HL_RT_VOID	8

/*
 * FlatBuffer FunctionCallType constants.
 */
#define HL_FCT_GUEST	1
#define HL_FCT_HOST	2

/*
 * FlatBuffer ReturnValue union discriminants.
 */
#define HL_RV_NONE	0
#define HL_RV_HLINT	1
#define HL_RV_HLUINT	2
#define HL_RV_HLLONG	3
#define HL_RV_HLULONG	4
#define HL_RV_HLFLOAT	5
#define HL_RV_HLDOUBLE	6
#define HL_RV_HLSTRING	7
#define HL_RV_HLBOOL	8
#define HL_RV_HLVOID	9

/*
 * FlatBuffer FunctionCallResultType discriminants.
 */
#define HL_FCRT_NONE		0
#define HL_FCRT_RETURN_VALUE	1
#define HL_FCRT_GUEST_ERROR	2

#endif /* __HYPERLIGHT_HCALL_H__ */
