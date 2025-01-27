/* -*- mode: C++; c-basic-offset: 4; -*- 
 *
 * Copyright (c) 2008-2011 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 * 
 * 
 *  Implements setjump-longjump based C++ exceptions
 * 
 */
#if __arm__

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <setjmp.h>
#if !FOR_DYLD
	#include <System/pthread_machdep.h>
#endif
#include "unwind.h"
#include "InternalMacros.h"

//
// ARM uses setjump/longjump based C++ exceptions.
// Other architectures use "zero cost" exceptions.
// 
// With SJLJ based exceptions any function that has a catch clause or needs to do any clean up when
// an exception propagates through it, needs to call _Unwind_SjLj_Register() at the start of the
// function and _Unwind_SjLj_Unregister() at the end.  The register function is called with the
// address of a block of memory in the function's stack frame.  The runtime keeps a linked list
// (stack) of these blocks - one per thread.  The calling function also sets the personality
// and lsda fields of the block. 
//
//

struct _Unwind_FunctionContext
{
	// next function in stack of handlers
	struct _Unwind_FunctionContext*		prev;

	// set by calling function before registering to be the landing pad
	uintptr_t							resumeLocation;
	
	// set by personality handler to be parameters passed to landing pad function
	uintptr_t							resumeParameters[4];

	// set by calling function before registering
	__personality_routine				personality;	// arm offset=24	
	uintptr_t							lsda;			// arm offset=28

	// variable length array, contains registers to restore
	// 0 = r7, 1 = pc, 2 = sp
	void*								jbuf[];
};


#if FOR_DYLD
	// implemented in dyld 
	extern struct _Unwind_FunctionContext* __Unwind_SjLj_GetTopOfFunctionStack(); 
	extern void __Unwind_SjLj_SetTopOfFunctionStack(struct _Unwind_FunctionContext* fc);
#else
	static struct _Unwind_FunctionContext* __Unwind_SjLj_GetTopOfFunctionStack()
	{
		return (struct _Unwind_FunctionContext*)_pthread_getspecific_direct(__PTK_LIBC_DYLD_Unwind_SjLj_Key);
	}

	static void __Unwind_SjLj_SetTopOfFunctionStack(struct _Unwind_FunctionContext* fc)
	{
		_pthread_setspecific_direct(__PTK_LIBC_DYLD_Unwind_SjLj_Key, fc);
	}
#endif


//
// Called at start of each function that catches exceptions
//
EXPORT void	_Unwind_SjLj_Register(struct _Unwind_FunctionContext* fc)
{
	fc->prev = __Unwind_SjLj_GetTopOfFunctionStack();
	__Unwind_SjLj_SetTopOfFunctionStack(fc);
}


//
// Called at end of each function that catches exceptions
//
EXPORT void	_Unwind_SjLj_Unregister(struct _Unwind_FunctionContext* fc)
{
	__Unwind_SjLj_SetTopOfFunctionStack(fc->prev);
}


static _Unwind_Reason_Code unwind_phase1(struct _Unwind_Exception* exception_object)
{
	_Unwind_FunctionContext_t c = __Unwind_SjLj_GetTopOfFunctionStack();
	DEBUG_PRINT_UNWINDING("unwind_phase1: initial function-context=%p\n", c); 

	// walk each frame looking for a place to stop
	for (bool handlerNotFound = true; handlerNotFound; c = c->prev) {

		// check for no more frames
		if ( c == NULL ) {
			DEBUG_PRINT_UNWINDING("unwind_phase1(ex_ojb=%p): reached bottom => _URC_END_OF_STACK\n", exception_object); 
			return _URC_END_OF_STACK;
		}
		
		DEBUG_PRINT_UNWINDING("unwind_phase1: function-context=%p\n", c); 
		// if there is a personality routine, ask it if it will want to stop at this frame
		if ( c->personality != NULL ) {
			DEBUG_PRINT_UNWINDING("unwind_phase1(ex_ojb=%p): calling personality function %p\n", exception_object, c->personality);
			_Unwind_Reason_Code personalityResult = (*c->personality)(1, _UA_SEARCH_PHASE, 
						exception_object->exception_class, exception_object, 
						(struct _Unwind_Context*)c);
			switch ( personalityResult ) {
				case _URC_HANDLER_FOUND:
					// found a catch clause or locals that need destructing in this frame
					// stop search and remember function context
					handlerNotFound = false;
					exception_object->private_2 = (uintptr_t)c;
					DEBUG_PRINT_UNWINDING("unwind_phase1(ex_ojb=%p): _URC_HANDLER_FOUND\n", exception_object);
					return _URC_NO_REASON;
					
				case _URC_CONTINUE_UNWIND:
					DEBUG_PRINT_UNWINDING("unwind_phase1(ex_ojb=%p): _URC_CONTINUE_UNWIND\n", exception_object);
					// continue unwinding
					break;
					
				default:
					// something went wrong
					DEBUG_PRINT_UNWINDING("unwind_phase1(ex_ojb=%p): _URC_FATAL_PHASE1_ERROR\n", exception_object);
					return _URC_FATAL_PHASE1_ERROR;
			}
		}
	}
	return _URC_NO_REASON;
}


static _Unwind_Reason_Code unwind_phase2(struct _Unwind_Exception* exception_object)
{
	DEBUG_PRINT_UNWINDING("unwind_phase2(ex_ojb=%p)\n", exception_object); 
	
	// walk each frame until we reach where search phase said to stop
	_Unwind_FunctionContext_t c = __Unwind_SjLj_GetTopOfFunctionStack();
	while ( true ) {
		DEBUG_PRINT_UNWINDING("unwind_phase2s(ex_ojb=%p): function-context=%p\n", exception_object, c); 

		// check for no more frames
		if ( c == NULL ) {
			DEBUG_PRINT_UNWINDING("unwind_phase2(ex_ojb=%p): unw_step() reached bottom => _URC_END_OF_STACK\n", exception_object); 
			return _URC_END_OF_STACK;
		}
		
		// if there is a personality routine, tell it we are unwinding
		if ( c->personality != NULL ) {
			_Unwind_Action action = _UA_CLEANUP_PHASE;
			if ( (uintptr_t)c == exception_object->private_2 )
				action = (_Unwind_Action)(_UA_CLEANUP_PHASE|_UA_HANDLER_FRAME); // tell personality this was the frame it marked in phase 1
			_Unwind_Reason_Code personalityResult = (*c->personality)(1, action, 
						exception_object->exception_class, exception_object, 
						(struct _Unwind_Context*)c);
			switch ( personalityResult ) {
				case _URC_CONTINUE_UNWIND:
					// continue unwinding
					DEBUG_PRINT_UNWINDING("unwind_phase2(ex_ojb=%p): _URC_CONTINUE_UNWIND\n", exception_object);
					if ( (uintptr_t)c == exception_object->private_2 ) {
						// phase 1 said we would stop at this frame, but we did not...
						ABORT("during phase1 personality function said it would stop here, but now if phase2 it did not stop here");
					}
					break;
				case _URC_INSTALL_CONTEXT:
					DEBUG_PRINT_UNWINDING("unwind_phase2(ex_ojb=%p): _URC_INSTALL_CONTEXT, will resume at landing pad %p\n", exception_object, c->jbuf[1]);
					// personality routine says to transfer control to landing pad
					// we may get control back if landing pad calls _Unwind_Resume()
					__Unwind_SjLj_SetTopOfFunctionStack(c);
					__builtin_longjmp(c->jbuf, 1);
					// unw_resume() only returns if there was an error
					return _URC_FATAL_PHASE2_ERROR;
				default:
					// something went wrong
					DEBUG_MESSAGE("personality function returned unknown result %d", personalityResult);
					return _URC_FATAL_PHASE2_ERROR;
			}
		}
		c = c->prev;
	}

	// clean up phase did not resume at the frame that the search phase said it would
	return _URC_FATAL_PHASE2_ERROR;
}


static _Unwind_Reason_Code unwind_phase2_forced(struct _Unwind_Exception* exception_object, 
												_Unwind_Stop_Fn stop, void* stop_parameter)
{
	// walk each frame until we reach where search phase said to stop
	_Unwind_FunctionContext_t c = __Unwind_SjLj_GetTopOfFunctionStack();
	while ( true ) {

		// get next frame (skip over first which is _Unwind_RaiseException)
		if ( c == NULL ) {
			DEBUG_PRINT_UNWINDING("unwind_phase2(ex_ojb=%p): unw_step() reached bottom => _URC_END_OF_STACK\n", exception_object); 
			return _URC_END_OF_STACK;
		}
				
		// call stop function at each frame
		_Unwind_Action action = (_Unwind_Action)(_UA_FORCE_UNWIND|_UA_CLEANUP_PHASE);
		_Unwind_Reason_Code stopResult = (*stop)(1, action, 
						exception_object->exception_class, exception_object, 
						(struct _Unwind_Context*)c, stop_parameter);
		DEBUG_PRINT_UNWINDING("unwind_phase2_forced(ex_ojb=%p): stop function returned %d\n", exception_object, stopResult);
		if ( stopResult != _URC_NO_REASON ) {
			DEBUG_PRINT_UNWINDING("unwind_phase2_forced(ex_ojb=%p): stopped by stop function\n", exception_object);
			return _URC_FATAL_PHASE2_ERROR;
		}
		
		// if there is a personality routine, tell it we are unwinding
		if ( c->personality != NULL ) {
			__personality_routine p = (__personality_routine)c->personality;
			DEBUG_PRINT_UNWINDING("unwind_phase2_forced(ex_ojb=%p): calling personality function %p\n", exception_object, p);
			_Unwind_Reason_Code personalityResult = (*p)(1, action, 
						exception_object->exception_class, exception_object, 
						(struct _Unwind_Context*)c);
			switch ( personalityResult ) {
				case _URC_CONTINUE_UNWIND:
					DEBUG_PRINT_UNWINDING("unwind_phase2_forced(ex_ojb=%p): personality returned _URC_CONTINUE_UNWIND\n", exception_object);
					// destructors called, continue unwinding
					break;
				case _URC_INSTALL_CONTEXT:
					DEBUG_PRINT_UNWINDING("unwind_phase2_forced(ex_ojb=%p): personality returned _URC_INSTALL_CONTEXT\n", exception_object);
					// we may get control back if landing pad calls _Unwind_Resume()
					__Unwind_SjLj_SetTopOfFunctionStack(c);
					__builtin_longjmp(c->jbuf, 1);
					break;
				default:
					// something went wrong
					DEBUG_PRINT_UNWINDING("unwind_phase2_forced(ex_ojb=%p): personality returned %d, _URC_FATAL_PHASE2_ERROR\n", 
						exception_object, personalityResult);
					return _URC_FATAL_PHASE2_ERROR;
			}
		}
		c = c->prev;
	}

	// call stop function one last time and tell it we've reached the end of the stack
	DEBUG_PRINT_UNWINDING("unwind_phase2_forced(ex_ojb=%p): calling stop function with _UA_END_OF_STACK\n", exception_object);
	_Unwind_Action lastAction = (_Unwind_Action)(_UA_FORCE_UNWIND|_UA_CLEANUP_PHASE|_UA_END_OF_STACK);
	(*stop)(1, lastAction, exception_object->exception_class, exception_object, (struct _Unwind_Context*)c, stop_parameter);
	
	// clean up phase did not resume at the frame that the search phase said it would
	return _URC_FATAL_PHASE2_ERROR;
}


//
// Called by __cxa_throw.  Only returns if there is a fatal error
//
EXPORT _Unwind_Reason_Code _Unwind_SjLj_RaiseException(struct _Unwind_Exception* exception_object)
{
	DEBUG_PRINT_API("_Unwind_SjLj_RaiseException(ex_obj=%p)\n", exception_object);

	// mark that this is a non-forced unwind, so _Unwind_Resume() can do the right thing
	exception_object->private_1	= 0;
	exception_object->private_2	= 0;

	// phase 1: the search phase
	_Unwind_Reason_Code phase1 = unwind_phase1(exception_object);
	if ( phase1 != _URC_NO_REASON )
		return phase1;
	
	// phase 2: the clean up phase
	return unwind_phase2(exception_object);  
}


//
// When _Unwind_RaiseException() is in phase2, it hands control
// to the personality function at each frame.  The personality
// may force a jump to a landing pad in that function, the landing
// pad code may then call _Unwind_Resume() to continue with the
// unwinding.  Note: the call to _Unwind_Resume() is from compiler
// geneated user code.  All other _Unwind_* routines are called 
// by the C++ runtime __cxa_* routines. 
//
// Re-throwing an exception is implemented by having the code call
// __cxa_rethrow() which in turn calls _Unwind_Resume_or_Rethrow()
//
EXPORT void _Unwind_SjLj_Resume(struct _Unwind_Exception* exception_object)
{
	DEBUG_PRINT_API("_Unwind_SjLj_Resume(ex_obj=%p)\n", exception_object);
	
	if ( exception_object->private_1 != 0 ) 
		unwind_phase2_forced(exception_object, (_Unwind_Stop_Fn)exception_object->private_1, (void*)exception_object->private_2);  
	else
		unwind_phase2(exception_object);  
	
	// clients assume _Unwind_Resume() does not return, so all we can do is abort.
	ABORT("_Unwind_SjLj_Resume() can't return");
}


//
//  Called by __cxa_rethrow()
//
EXPORT _Unwind_Reason_Code _Unwind_SjLj_Resume_or_Rethrow(struct _Unwind_Exception* exception_object)
{
	DEBUG_PRINT_API("__Unwind_SjLj_Resume_or_Rethrow(ex_obj=%p), private_1=%ld\n", exception_object, exception_object->private_1);
	// if this is non-forced and a stopping place was found, then this is a re-throw
	// call _Unwind_RaiseException() as if this was a new exception
	if ( exception_object->private_1 == 0 ) {
		return _Unwind_SjLj_RaiseException(exception_object); 
		// should return if there is no catch clause, so that __cxa_rethrow can call std::terminate()
	}
	
	// call through to _Unwind_Resume() which distiguishes between forced and regular exceptions
	_Unwind_SjLj_Resume(exception_object); 
	ABORT("__Unwind_SjLj_Resume_or_Rethrow() called _Unwind_SjLj_Resume() which unexpectedly returned");
}


//
// Called by personality handler during phase 2 to get LSDA for current frame
//
EXPORT uintptr_t _Unwind_GetLanguageSpecificData(struct _Unwind_Context* context)
{
	_Unwind_FunctionContext_t ufc = (_Unwind_FunctionContext_t)context;
	DEBUG_PRINT_API("_Unwind_GetLanguageSpecificData(context=%p) => 0x%0lX\n", context, ufc->lsda);
	return ufc->lsda;
}


//
// Called by personality handler during phase 2 to get register values
//
EXPORT uintptr_t _Unwind_GetGR(struct _Unwind_Context* context, int index)
{
	DEBUG_PRINT_API("_Unwind_GetGR(context=%p, reg=%d)\n", context, index);
	_Unwind_FunctionContext_t ufc = (_Unwind_FunctionContext_t)context;
	return ufc->resumeParameters[index];
}



//
// Called by personality handler during phase 2 to alter register values
//
EXPORT void _Unwind_SetGR(struct _Unwind_Context* context, int index, uintptr_t new_value)
{
	DEBUG_PRINT_API("_Unwind_SetGR(context=%p, reg=%d, value=0x%0lX)\n", context, index, new_value);
	_Unwind_FunctionContext_t ufc = (_Unwind_FunctionContext_t)context;
	ufc->resumeParameters[index] = new_value;
}


//
// Called by personality handler during phase 2 to get instruction pointer
//
EXPORT uintptr_t _Unwind_GetIP(struct _Unwind_Context* context)
{
	_Unwind_FunctionContext_t ufc = (_Unwind_FunctionContext_t)context;
	DEBUG_PRINT_API("_Unwind_GetIP(context=%p) => 0x%lX\n", context, ufc->resumeLocation+1);
	return ufc->resumeLocation+1;
}

//
// Called by personality handler during phase 2 to get instruction pointer
// ipBefore is a boolean that says if IP is already adjusted to be the call
// site address.  Normally IP is the return address.
//
EXPORT uintptr_t _Unwind_GetIPInfo(struct _Unwind_Context* context, int* ipBefore)
{
	_Unwind_FunctionContext_t ufc = (_Unwind_FunctionContext_t)context;
	*ipBefore = 0;
	DEBUG_PRINT_API("_Unwind_GetIPInfo(context=%p, %p) => 0x%lX\n", context, ipBefore, ufc->resumeLocation+1);
	return ufc->resumeLocation+1; 
}


//
// Called by personality handler during phase 2 to alter instruction pointer
//
EXPORT void _Unwind_SetIP(struct _Unwind_Context* context, uintptr_t new_value)
{
	DEBUG_PRINT_API("_Unwind_SetIP(context=%p, value=0x%0lX)\n", context, new_value);
	_Unwind_FunctionContext_t ufc = (_Unwind_FunctionContext_t)context;
	ufc->resumeLocation = new_value-1;
}

//
// Called by personality handler during phase 2 to find the start of the function
//
EXPORT uintptr_t _Unwind_GetRegionStart(struct _Unwind_Context* context)
{
	// Not supported or needed for sjlj based unwinding
	DEBUG_PRINT_API("_Unwind_GetRegionStart(context=%p)\n", context);
	return 0;
}

//
// Called by personality handler during phase 2 if a foreign exception is caught 
//
EXPORT void _Unwind_DeleteException(struct _Unwind_Exception* exception_object)
{
	DEBUG_PRINT_API("_Unwind_DeleteException(ex_obj=%p)\n", exception_object);
	if ( exception_object->exception_cleanup != NULL )
		(*exception_object->exception_cleanup)(_URC_FOREIGN_EXCEPTION_CAUGHT, exception_object);
}


//
// Called by personality handler during phase 2 to get base address for data relative encodings
//
EXPORT uintptr_t _Unwind_GetDataRelBase(struct _Unwind_Context* context)
{
	// Not supported or needed for sjlj based unwinding
	DEBUG_PRINT_API("_Unwind_GetDataRelBase(context=%p)\n", context);
	ABORT("_Unwind_GetDataRelBase() not implemented");
}


//
// Called by personality handler during phase 2 to get base address for text relative encodings
//
EXPORT uintptr_t _Unwind_GetTextRelBase(struct _Unwind_Context* context)
{
	// Not supported or needed for sjlj based unwinding
	DEBUG_PRINT_API("_Unwind_GetTextRelBase(context=%p)\n", context);
	ABORT("_Unwind_GetTextRelBase() not implemented");
}



//
// Called by personality handler to get Call Frame Area for current frame
//
EXPORT uintptr_t _Unwind_GetCFA(struct _Unwind_Context* context)
{
	DEBUG_PRINT_API("_Unwind_GetCFA(context=%p)\n", context);
	if ( context != NULL ) {
		_Unwind_FunctionContext_t ufc = (_Unwind_FunctionContext_t)context;
		// setjmp/longjmp based exceptions don't have a true CFA
		// the SP in the jmpbuf is the closest approximation
		return (uintptr_t)ufc->jbuf[2];
	}
	return 0;
}



#if !FOR_DYLD && __IPHONE_OS_VERSION_MIN_REQUIRED
//
// symbols in libSystem.dylib in iOS 5.0 and later, but are in libgcc_s.dylib in earlier versions
//
NOT_HERE_BEFORE_5_0(_Unwind_GetLanguageSpecificData)
NOT_HERE_BEFORE_5_0(_Unwind_GetRegionStart)
NOT_HERE_BEFORE_5_0(_Unwind_GetIP)
NOT_HERE_BEFORE_5_0(_Unwind_SetGR)
NOT_HERE_BEFORE_5_0(_Unwind_SetIP)
NOT_HERE_BEFORE_5_0(_Unwind_DeleteException)
NOT_HERE_BEFORE_5_0(_Unwind_SjLj_Register)
NOT_HERE_BEFORE_5_0(_Unwind_GetGR)
NOT_HERE_BEFORE_5_0(_Unwind_GetIPInfo)
NOT_HERE_BEFORE_5_0(_Unwind_GetCFA)
NOT_HERE_BEFORE_5_0(_Unwind_SjLj_Resume)
NOT_HERE_BEFORE_5_0(_Unwind_SjLj_RaiseException)
NOT_HERE_BEFORE_5_0(_Unwind_SjLj_Resume_or_Rethrow)
NOT_HERE_BEFORE_5_0(_Unwind_SjLj_Unregister)
#endif //  !FOR_DYLD


#endif // __arm__
