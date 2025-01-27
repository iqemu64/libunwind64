/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2007-2009 Apple Inc. All rights reserved.
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
 */
 
//
//	C++ interface to lower levels of libuwind 
//

#ifndef __UNWINDCURSOR_HPP__
#define __UNWINDCURSOR_HPP__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <Availability.h>
#include <setjmp.h>

#include "libunwind.h"
#include "libiqemu.h"

#include "AddressSpace.hpp"
#include "Registers.hpp"
#include "DwarfInstructions.hpp"
#include "CompactUnwinder.hpp"
#include "InternalMacros.h"

#if __MAC_OS_X_VERSION_MIN_REQUIRED
  #define KEYMGR_SUPPPORT 1
#else
  #define KEYMGR_SUPPPORT 0
#endif


#if KEYMGR_SUPPPORT
// private keymgr stuff
#define KEYMGR_GCC3_DW2_OBJ_LIST   302  
extern "C" {
	extern void	 _keymgr_set_and_unlock_processwide_ptr(int key, void* ptr);
	extern void* _keymgr_get_and_lock_processwide_ptr(int key);
};

// undocumented libgcc "struct object"
struct libgcc_object 
{
	void*			start;
	void*			unused1;
	void*			unused2;
	void*			fde;
	unsigned long	encoding;
	void*			fde_end;
	libgcc_object*	next;
};

// undocumented libgcc "struct km_object_info" referenced by KEYMGR_GCC3_DW2_OBJ_LIST
struct libgcc_object_info {
  struct libgcc_object*		seen_objects;
  struct libgcc_object*		unseen_objects;
  unsigned					spare[2];
};
#endif // KEYMGR_SUPPPORT



namespace libunwind {

#if !FOR_DYLD
template <typename A>
class DwarfFDECache 
{
public:
	typedef typename A::pint_t	pint_t;
	static pint_t					findFDE(pint_t mh, pint_t pc);
	static void						add(pint_t mh, pint_t ip_start, pint_t ip_end, pint_t fde);
	static void						removeAllIn(pint_t mh);
	static void						iterateCacheEntries(void (*func)(unw_word_t ip_start, unw_word_t ip_end, unw_word_t fde, unw_word_t mh));
private:
	static void						dyldUnloadHook(const struct mach_header* mh, intptr_t vmaddr_slide);
	
	struct entry { pint_t mh; pint_t ip_start; pint_t ip_end; pint_t fde; };

	// these fields are all static to avoid needing an initializer
	// there is only one instance of this class per process
	static pthread_rwlock_t			fgLock;	
	static bool						fgRegisteredForDyldUnloads;	
	// can't use std::vector<> here because this code must live in libSystem.dylib (which is below libstdc++.dylib)
	static entry*					fgBuffer;
	static entry*					fgBufferUsed;
	static entry*					fgBufferEnd;
	static entry					fgInitialBuffer[64];
};

template <typename A> typename DwarfFDECache<A>::entry* DwarfFDECache<A>::fgBuffer		= fgInitialBuffer;
template <typename A> typename DwarfFDECache<A>::entry* DwarfFDECache<A>::fgBufferUsed	= fgInitialBuffer;
template <typename A> typename DwarfFDECache<A>::entry* DwarfFDECache<A>::fgBufferEnd	= &fgInitialBuffer[64];
template <typename A> typename DwarfFDECache<A>::entry  DwarfFDECache<A>::fgInitialBuffer[64];

template <typename A>
pthread_rwlock_t DwarfFDECache<A>::fgLock = PTHREAD_RWLOCK_INITIALIZER;

template <typename A> 
bool DwarfFDECache<A>::fgRegisteredForDyldUnloads = false;


template <typename A>
typename A::pint_t DwarfFDECache<A>::findFDE(pint_t mh, pint_t pc)
{
	pint_t result = NULL;
	DEBUG_LOG_NON_ZERO(::pthread_rwlock_rdlock(&fgLock));
	for(entry* p=fgBuffer; p < fgBufferUsed; ++p) {
		if ( (mh == p->mh) || (mh == 0) ) {
			if ( (p->ip_start <= pc) && (pc < p->ip_end) ) {
				result = p->fde;
				break;
			}
		}
	}
	DEBUG_LOG_NON_ZERO(::pthread_rwlock_unlock(&fgLock));
	//fprintf(stderr, "DwarfFDECache::findFDE(mh=0x%llX, pc=0x%llX) => 0x%llX\n", (uint64_t)mh, (uint64_t)pc, (uint64_t)result);
	return result;
}

template <typename A>
void DwarfFDECache<A>::add(pint_t mh, pint_t ip_start, pint_t ip_end, pint_t fde)
{
	//fprintf(stderr, "DwarfFDECache::add(mh=0x%llX, ip_start=0x%llX, ip_end=0x%llX, fde=0x%llX) pthread=%p\n", 
	//		(uint64_t)mh, (uint64_t)ip_start, (uint64_t)ip_end, (uint64_t)fde, pthread_self());
	DEBUG_LOG_NON_ZERO(::pthread_rwlock_wrlock(&fgLock));
	if ( fgBufferUsed >= fgBufferEnd ) {
		int oldSize = fgBufferEnd - fgBuffer;
		int newSize = oldSize*4;
		entry* newBuffer = (entry*)malloc(newSize*sizeof(entry));	// can't use operator new in libSystem.dylib
		memcpy(newBuffer, fgBuffer, oldSize*sizeof(entry));
		//fprintf(stderr, "DwarfFDECache::add() growing buffer to %d\n",  newSize);
		if ( fgBuffer != fgInitialBuffer )
			free(fgBuffer);
		fgBuffer = newBuffer;
		fgBufferUsed = &newBuffer[oldSize];
		fgBufferEnd = &newBuffer[newSize];
	}
	fgBufferUsed->mh = mh;
	fgBufferUsed->ip_start = ip_start;
	fgBufferUsed->ip_end = ip_end;
	fgBufferUsed->fde = fde;
	++fgBufferUsed;
	if ( !fgRegisteredForDyldUnloads ) {
		_dyld_register_func_for_remove_image(&dyldUnloadHook);
		fgRegisteredForDyldUnloads = true;
	}
	DEBUG_LOG_NON_ZERO(::pthread_rwlock_unlock(&fgLock));
}



template <typename A>
void DwarfFDECache<A>::removeAllIn(pint_t mh)
{
	DEBUG_LOG_NON_ZERO(::pthread_rwlock_wrlock(&fgLock));
	entry* d=fgBuffer;
	for(const entry* s=fgBuffer; s < fgBufferUsed; ++s) {
		if ( s->mh != mh ) {
			if ( d != s ) 
				*d = *s;
			++d;
		}
	}
	fgBufferUsed = d;
	DEBUG_LOG_NON_ZERO(::pthread_rwlock_unlock(&fgLock));
}


template <typename A>
void DwarfFDECache<A>::dyldUnloadHook(const struct mach_header* mh, intptr_t vmaddr_slide)
{
	removeAllIn((pint_t)mh);
}

template <typename A>
void DwarfFDECache<A>::iterateCacheEntries(void (*func)(unw_word_t ip_start, unw_word_t ip_end, unw_word_t fde, unw_word_t mh))
{
	DEBUG_LOG_NON_ZERO(::pthread_rwlock_wrlock(&fgLock));
	for(entry* p=fgBuffer; p < fgBufferUsed; ++p) {
		(*func)(p->ip_start, p->ip_end, p->fde, p->mh);
	}
	DEBUG_LOG_NON_ZERO(::pthread_rwlock_unlock(&fgLock));
}
#endif // !FOR_DYLD




#define arrayoffsetof(type, index, field) ((size_t)(&((type *)0)[index].field))

template <typename A>
class UnwindSectionHeader {
public:
					UnwindSectionHeader(A& addressSpace, typename A::pint_t addr) : fAddressSpace(addressSpace), fAddr(addr) {}

	uint32_t		version() const								INLINE { return fAddressSpace.get32(fAddr + offsetof(unwind_info_section_header, version)); }
	uint32_t		commonEncodingsArraySectionOffset() const	INLINE { return fAddressSpace.get32(fAddr + offsetof(unwind_info_section_header, commonEncodingsArraySectionOffset)); }
	uint32_t		commonEncodingsArrayCount() const			INLINE { return fAddressSpace.get32(fAddr + offsetof(unwind_info_section_header, commonEncodingsArrayCount)); }
	uint32_t		personalityArraySectionOffset() const		INLINE { return fAddressSpace.get32(fAddr + offsetof(unwind_info_section_header, personalityArraySectionOffset)); }
	uint32_t		personalityArrayCount() const				INLINE { return fAddressSpace.get32(fAddr + offsetof(unwind_info_section_header, personalityArrayCount)); }
	uint32_t		indexSectionOffset() const					INLINE { return fAddressSpace.get32(fAddr + offsetof(unwind_info_section_header, indexSectionOffset)); }
	uint32_t		indexCount() const							INLINE { return fAddressSpace.get32(fAddr + offsetof(unwind_info_section_header, indexCount)); }
private:
	A&						fAddressSpace;
	typename A::pint_t		fAddr;
};

template <typename A>
class UnwindSectionIndexArray {
public:
					UnwindSectionIndexArray(A& addressSpace, typename A::pint_t addr) : fAddressSpace(addressSpace), fAddr(addr) {}

	uint32_t		functionOffset(int index) const					INLINE { return fAddressSpace.get32(fAddr + arrayoffsetof(unwind_info_section_header_index_entry, index, functionOffset)); }
	uint32_t		secondLevelPagesSectionOffset(int index) const	INLINE { return fAddressSpace.get32(fAddr + arrayoffsetof(unwind_info_section_header_index_entry, index, secondLevelPagesSectionOffset)); }
	uint32_t		lsdaIndexArraySectionOffset(int index) const	INLINE { return fAddressSpace.get32(fAddr + arrayoffsetof(unwind_info_section_header_index_entry, index, lsdaIndexArraySectionOffset)); }
private:
	A&						fAddressSpace;
	typename A::pint_t		fAddr;
};


template <typename A>
class UnwindSectionRegularPageHeader {
public:
					UnwindSectionRegularPageHeader(A& addressSpace, typename A::pint_t addr) : fAddressSpace(addressSpace), fAddr(addr) {}

	uint32_t		kind() const				INLINE { return fAddressSpace.get32(fAddr + offsetof(unwind_info_regular_second_level_page_header, kind)); }
	uint16_t		entryPageOffset() const		INLINE { return fAddressSpace.get16(fAddr + offsetof(unwind_info_regular_second_level_page_header, entryPageOffset)); }
	uint16_t		entryCount() const			INLINE { return fAddressSpace.get16(fAddr + offsetof(unwind_info_regular_second_level_page_header, entryCount)); }
private:
	A&						fAddressSpace;
	typename A::pint_t		fAddr;
};


template <typename A>
class UnwindSectionRegularArray {
public:
					UnwindSectionRegularArray(A& addressSpace, typename A::pint_t addr) : fAddressSpace(addressSpace), fAddr(addr) {}

	uint32_t		functionOffset(int index) const		INLINE { return fAddressSpace.get32(fAddr + arrayoffsetof(unwind_info_regular_second_level_entry, index, functionOffset)); }
	uint32_t		encoding(int index) const			INLINE { return fAddressSpace.get32(fAddr + arrayoffsetof(unwind_info_regular_second_level_entry, index, encoding)); }
private:
	A&						fAddressSpace;
	typename A::pint_t		fAddr;
};


template <typename A>
class UnwindSectionCompressedPageHeader {
public:
					UnwindSectionCompressedPageHeader(A& addressSpace, typename A::pint_t addr) : fAddressSpace(addressSpace), fAddr(addr) {}

	uint32_t		kind() const				INLINE { return fAddressSpace.get32(fAddr + offsetof(unwind_info_compressed_second_level_page_header, kind)); }
	uint16_t		entryPageOffset() const		INLINE { return fAddressSpace.get16(fAddr + offsetof(unwind_info_compressed_second_level_page_header, entryPageOffset)); }
	uint16_t		entryCount() const			INLINE { return fAddressSpace.get16(fAddr + offsetof(unwind_info_compressed_second_level_page_header, entryCount)); }
	uint16_t		encodingsPageOffset() const	INLINE { return fAddressSpace.get16(fAddr + offsetof(unwind_info_compressed_second_level_page_header, encodingsPageOffset)); }
	uint16_t		encodingsCount() const		INLINE { return fAddressSpace.get16(fAddr + offsetof(unwind_info_compressed_second_level_page_header, encodingsCount)); }
private:
	A&						fAddressSpace;
	typename A::pint_t		fAddr;
};


template <typename A>
class UnwindSectionCompressedArray {
public:
					UnwindSectionCompressedArray(A& addressSpace, typename A::pint_t addr) : fAddressSpace(addressSpace), fAddr(addr) {}

	uint32_t		functionOffset(int index) const		INLINE { return UNWIND_INFO_COMPRESSED_ENTRY_FUNC_OFFSET( fAddressSpace.get32(fAddr + index*sizeof(uint32_t)) ); }
	uint16_t		encodingIndex(int index) const		INLINE { return UNWIND_INFO_COMPRESSED_ENTRY_ENCODING_INDEX( fAddressSpace.get32(fAddr + index*sizeof(uint32_t)) ); }
private:
	A&						fAddressSpace;
	typename A::pint_t		fAddr;
};


template <typename A>
class UnwindSectionLsdaArray {
public:
					UnwindSectionLsdaArray(A& addressSpace, typename A::pint_t addr) : fAddressSpace(addressSpace), fAddr(addr) {}

	uint32_t		functionOffset(int index) const		INLINE { return fAddressSpace.get32(fAddr + arrayoffsetof(unwind_info_section_header_lsda_index_entry, index, functionOffset)); }
	int32_t			lsdaOffset(int index) const			INLINE { return fAddressSpace.get32(fAddr + arrayoffsetof(unwind_info_section_header_lsda_index_entry, index, lsdaOffset)); }
private:
	A&						fAddressSpace;
	typename A::pint_t		fAddr;
};


template <typename A, typename R, typename R2 = R>
class UnwindCursor
{
public:
						UnwindCursor(unw_context_t* context, A& as);
						UnwindCursor(A& as, thread_t thread);
	virtual				~UnwindCursor() {}
	virtual bool		validReg(int);
	virtual uint64_t	getReg(int);
	virtual void		setReg(int, uint64_t);
	virtual bool		validFloatReg(int);
	virtual double		getFloatReg(int);
	virtual void		setFloatReg(int, double);
	virtual int			step();
	virtual void		getInfo(unw_proc_info_t*);
	virtual void		jumpto();
	virtual const char*	getRegisterName(int num);
	virtual bool		isSignalFrame();
	virtual bool		getFunctionName(char* buf, size_t bufLen, unw_word_t* offset);
	virtual void		setInfoBasedOnIPRegister(bool isReturnAddress=false);

	void				operator delete(void* p, size_t size) {}
    
    virtual void        setJmpBuf(int *jmpbuf) {
        switch(fRegSetNo) {
            case Regset_No_1:
                fRegisters.setJmpbuf(jmpbuf);
                break;
            case Regset_No_2:
                fRegisters2.setJmpbuf(jmpbuf);
                break;
            default:
                abort();
        }
    }

private:
	typedef typename A::pint_t		pint_t;	
	typedef uint32_t				EncodedUnwindInfo;

    virtual int      switchArchIfNecessary(pint_t &pc);
    
	bool				getInfoFromCompactEncodingSection(pint_t pc, pint_t mh, pint_t unwindSectionStart);
	bool				getInfoFromDwarfSection(pint_t pc, pint_t mh, pint_t ehSectionStart, uint32_t sectionLength, uint32_t sectionOffsetOfFDE);

	int					stepWithDwarfFDE() {
                            switch(fRegSetNo) {
                            case Regset_No_1: return DwarfInstructions<A,R>::stepWithDwarf(fAddressSpace, this->getReg(UNW_REG_IP), fInfo.unwind_info, fRegisters);
                            case Regset_No_2: return DwarfInstructions<A,R2>::stepWithDwarf(fAddressSpace, this->getReg(UNW_REG_IP), fInfo.unwind_info, fRegisters2);
                            default: abort();
                            }
                        }
    int                 stepWithCompactEncoding() {
                            switch(fRegSetNo) {
                            case Regset_No_1: { R dummy; return stepWithCompactEncoding(dummy); }
                            case Regset_No_2: { R2 dummy; return stepWithCompactEncoding2(dummy); }
                            default: abort();
                            }
                        }
	int					stepWithCompactEncoding(Registers_x86_64&)
							{ return CompactUnwinder_x86_64<A>::stepWithCompactEncoding(fInfo.format, fInfo.start_ip, fAddressSpace, fRegisters); }
	int					stepWithCompactEncoding(Registers_x86&) 
							{ return CompactUnwinder_x86<A>::stepWithCompactEncoding(fInfo.format, fInfo.start_ip, fAddressSpace, fRegisters); }
	int					stepWithCompactEncoding(Registers_ppc&) 
							{ return UNW_EINVAL; }
    int                 stepWithCompactEncoding(Registers_arm64&)
                            { return CompactUnwinder_arm64<A>::stepWithCompactEncoding(fInfo.format, fInfo.start_ip, fAddressSpace, fRegisters); }
    int                 stepWithCompactEncoding2(Registers_x86_64&)
                            { return CompactUnwinder_x86_64<A>::stepWithCompactEncoding(fInfo.format, fInfo.start_ip, fAddressSpace, fRegisters2); }
    int                 stepWithCompactEncoding2(Registers_x86&)
                            { return CompactUnwinder_x86<A>::stepWithCompactEncoding(fInfo.format, fInfo.start_ip, fAddressSpace, fRegisters2); }
    int                 stepWithCompactEncoding2(Registers_ppc&)
                            { return UNW_EINVAL; }
    int                 stepWithCompactEncoding2(Registers_arm64&)
                            { return CompactUnwinder_arm64<A>::stepWithCompactEncoding(fInfo.format, fInfo.start_ip, fAddressSpace, fRegisters2); }
#if FOR_DYLD
  #if __ppc__
	bool				mustUseDwarf() const { return true; }
  #else
	bool				mustUseDwarf() const { return false; }
  #endif
#else
	bool				mustUseDwarf() const {
                            switch(fRegSetNo) {
                                case Regset_No_1: { R dummy; uint32_t offset; return dwarfWithOffset(dummy, offset); }
                                case Regset_No_2: { R2 dummy; uint32_t offset; return dwarfWithOffset(dummy, offset); }
                                default: abort();
                            }
                        }
#endif
	bool				dwarfWithOffset(uint32_t& offset) const {
                            switch(fRegSetNo) {
                                case Regset_No_1: { R dummy; return dwarfWithOffset(dummy, offset); }
                                case Regset_No_2: { R2 dummy; return dwarfWithOffset(dummy, offset); }
                                default: abort();
                            }
                        }
	bool				dwarfWithOffset(Registers_x86_64&, uint32_t& offset) const { 
							if ( (fInfo.format & UNWIND_X86_64_MODE_MASK) == UNWIND_X86_64_MODE_DWARF ) {
								offset = (fInfo.format & UNWIND_X86_64_DWARF_SECTION_OFFSET);
								return true;
							}
#if SUPPORT_OLD_BINARIES
							if ( (fInfo.format & UNWIND_X86_64_MODE_MASK) == UNWIND_X86_64_MODE_COMPATIBILITY ) {
								if ( (fInfo.format & UNWIND_X86_64_CASE_MASK) == UNWIND_X86_64_UNWIND_REQUIRES_DWARF ) {
									offset = 0;
									return true;
								}
							}
#endif
							return false;
						}
	bool				dwarfWithOffset(Registers_x86&, uint32_t& offset) const { 
							if ( (fInfo.format & UNWIND_X86_MODE_MASK) == UNWIND_X86_MODE_DWARF ) {
								offset = (fInfo.format & UNWIND_X86_DWARF_SECTION_OFFSET);
								return true;
							}
#if SUPPORT_OLD_BINARIES
							if ( (fInfo.format & UNWIND_X86_MODE_MASK) == UNWIND_X86_MODE_COMPATIBILITY ) {
								if ( (fInfo.format & UNWIND_X86_CASE_MASK) == UNWIND_X86_UNWIND_REQUIRES_DWARF ) {
									offset = 0;
									return true;
								}
							}
#endif
							return false;
						}
	bool				dwarfWithOffset(Registers_ppc&, uint32_t& offset) const { return true; }
    bool                dwarfWithOffset(Registers_arm64&, uint32_t& offset) const {
        if((fInfo.format & UNWIND_ARM64_MODE_MASK) == UNWIND_ARM64_MODE_DWARF) {
            offset = (fInfo.format & UNWIND_ARM64_DWARF_SECTION_OFFSET);
            return true;
        }
        
        return false;
    }

	compact_unwind_encoding_t		dwarfEncoding() const {
                                        switch(fRegSetNo) {
                                            case Regset_No_1: { R dummy; return dwarfEncoding(dummy); }
                                            case Regset_No_2: { R2 dummy; return dwarfEncoding(dummy); }
                                            default: abort();
                                        }
                                    }

	compact_unwind_encoding_t		dwarfEncoding(Registers_x86_64&) const { return UNWIND_X86_64_MODE_DWARF; }
	compact_unwind_encoding_t		dwarfEncoding(Registers_x86&)	const { return UNWIND_X86_MODE_DWARF; }
	compact_unwind_encoding_t		dwarfEncoding(Registers_ppc&)	const { return 0; }
    compact_unwind_encoding_t       dwarfEncoding(Registers_arm64&) const { return UNWIND_ARM64_MODE_DWARF; }

	unw_proc_info_t				fInfo;
	R							fRegisters;
    R2                          fRegisters2;
	A&							fAddressSpace;
	bool						fUnwindInfoMissing;
	bool						fIsSignalFrame;
    enum reg_set_no_t {
        Regset_No_1,
        Regset_No_2,
    };
    
    reg_set_no_t                fRegSetNo;
    int                         fJmpBufIndex;
};

typedef UnwindCursor<LocalAddressSpace,Registers_x86_64,Registers_arm64> AbstractUnwindCursor;

template <typename A, typename R, typename R2>
UnwindCursor<A,R,R2>::UnwindCursor(unw_context_t* context, A& as)
  : fRegisters(context), fAddressSpace(as), fUnwindInfoMissing(false), fIsSignalFrame(false), fRegSetNo(Regset_No_1), fJmpBufIndex(0)
{
	COMPILE_TIME_ASSERT( sizeof(UnwindCursor<A,R,R2>) < sizeof(unw_cursor_t) );

	bzero(&fInfo, sizeof(fInfo));
    
    fRegisters2.initWithQemuContext();
}

template <typename A, typename R, typename R2>
UnwindCursor<A,R,R2>::UnwindCursor(A& as, thread_t thread)
  : fAddressSpace(as), fUnwindInfoMissing(false), fIsSignalFrame(false), fRegSetNo(Regset_No_1), fJmpBufIndex(0)
{
	bzero(&fInfo, sizeof(fInfo));
	// FIXME
	// fill in fRegisters from thread
}

template <typename A, typename R, typename R2>
bool UnwindCursor<A,R,R2>::validReg(int regNum)
{
    switch(fRegSetNo) {
    case Regset_No_1: return fRegisters.validRegister(regNum);
    case Regset_No_2: return fRegisters2.validRegister(regNum);
    default: abort();
    }
}

template <typename A, typename R, typename R2>
uint64_t UnwindCursor<A,R,R2>::getReg(int regNum)
{ 
	switch(fRegSetNo) {
    case Regset_No_1: return fRegisters.getRegister(regNum);
    case Regset_No_2: return fRegisters2.getRegister(regNum);
    default: abort();
    }
}

template <typename A, typename R, typename R2>
void UnwindCursor<A,R,R2>::setReg(int regNum, uint64_t value)
{ 
	switch(fRegSetNo) {
    case Regset_No_1: fRegisters.setRegister(regNum, value); break;
    case Regset_No_2: fRegisters2.setRegister(regNum, value); break;
    default: abort();
    }
}

template <typename A, typename R, typename R2>
bool UnwindCursor<A,R,R2>::validFloatReg(int regNum)
{
    switch(fRegSetNo) {
    case Regset_No_1: return fRegisters.validFloatRegister(regNum);
    case Regset_No_2: return fRegisters2.validFloatRegister(regNum);
    default: abort();
    }
}

template <typename A, typename R, typename R2>
double UnwindCursor<A,R,R2>::getFloatReg(int regNum)
{
    switch(fRegSetNo) {
    case Regset_No_1: return fRegisters.getFloatRegister(regNum);
    case Regset_No_2: return fRegisters2.getFloatRegister(regNum);
    default: abort();
    }
}

template <typename A, typename R, typename R2>
void UnwindCursor<A,R,R2>::setFloatReg(int regNum, double value)
{
    switch(fRegSetNo) {
    case Regset_No_1: fRegisters.setFloatRegister(regNum, value); break;
    case Regset_No_2: fRegisters2.setFloatRegister(regNum, value); break;
    default: abort();
    }
}

template <typename A, typename R, typename R2>
void UnwindCursor<A,R,R2>::jumpto()
{
    switch(fRegSetNo) {
    case Regset_No_1: fRegisters2.truncate_CFI(fJmpBufIndex); fRegisters.jumpto(); break;
    case Regset_No_2: fRegisters2.jumpto(); break;
    default: abort();
    }
}

template <typename A, typename R, typename R2>
const char* UnwindCursor<A,R,R2>::getRegisterName(int regNum)
{
    switch(fRegSetNo) {
    case Regset_No_1: return fRegisters.getRegisterName(regNum);
    case Regset_No_2: return fRegisters2.getRegisterName(regNum);
    default: abort();
    }
}

template <typename A, typename R, typename R2>
bool UnwindCursor<A,R,R2>::isSignalFrame()
{ 
	 return fIsSignalFrame;
}


template <typename A, typename R, typename R2>
bool UnwindCursor<A,R,R2>::getInfoFromDwarfSection(pint_t pc, pint_t mh, pint_t ehSectionStart, uint32_t sectionLength, uint32_t sectionOffsetOfFDE)
{
	typename CFI_Parser<A>::FDE_Info fdeInfo;
	typename CFI_Parser<A>::CIE_Info cieInfo;
	bool foundFDE = false;
	bool foundInCache = false;
	// if compact encoding table gave offset into dwarf section, go directly there
	if ( sectionOffsetOfFDE != 0 ) {
		foundFDE = CFI_Parser<A>::findFDE(fAddressSpace, pc, ehSectionStart, sectionLength, ehSectionStart+sectionOffsetOfFDE, &fdeInfo, &cieInfo);
	}
#if !FOR_DYLD
	if ( !foundFDE ) {
		// otherwise, search cache of previously found FDEs
		pint_t cachedFDE = DwarfFDECache<A>::findFDE(mh, pc);
		//fprintf(stderr, "getInfoFromDwarfSection(pc=0x%llX) cachedFDE=0x%llX\n", (uint64_t)pc, (uint64_t)cachedFDE);
		if ( cachedFDE != 0 ) {
			foundFDE = CFI_Parser<A>::findFDE(fAddressSpace, pc, ehSectionStart, sectionLength, cachedFDE, &fdeInfo, &cieInfo);
			foundInCache = foundFDE;
			//fprintf(stderr, "cachedFDE=0x%llX, foundInCache=%d\n", (uint64_t)cachedFDE, foundInCache);
		}
	}
#endif
	if ( !foundFDE ) {
		// still not found, do full scan of __eh_frame section
		foundFDE = CFI_Parser<A>::findFDE(fAddressSpace, pc, ehSectionStart, sectionLength, 0, &fdeInfo, &cieInfo);
	}
	if ( foundFDE ) {
		typename CFI_Parser<A>::PrologInfo prolog;
		if ( CFI_Parser<A>::parseFDEInstructions(fAddressSpace, fdeInfo, cieInfo, pc, &prolog) ) {
			// save off parsed FDE info
			fInfo.start_ip			= fdeInfo.pcStart;
			fInfo.end_ip			= fdeInfo.pcEnd;
			fInfo.lsda				= fdeInfo.lsda;
			fInfo.handler			= cieInfo.personality;
			fInfo.gp				= prolog.spExtraArgSize;  // some frameless functions need SP altered when resuming in function
			fInfo.flags				= 0;
			fInfo.format			= dwarfEncoding();  
			fInfo.unwind_info		= fdeInfo.fdeStart;
			fInfo.unwind_info_size	= fdeInfo.fdeLength;
			fInfo.extra				= (unw_word_t)mh;
			if ( !foundInCache && (sectionOffsetOfFDE == 0) ) {
				// don't add to cache entries the compact encoding table can find quickly
				//fprintf(stderr, "getInfoFromDwarfSection(pc=0x%0llX), mh=0x%llX, start_ip=0x%0llX, fde=0x%0llX, personality=0x%0llX\n", 
				//	(uint64_t)pc, (uint64_t)mh, fInfo.start_ip, fInfo.unwind_info, fInfo.handler);
#if !FOR_DYLD
				DwarfFDECache<A>::add(mh, fdeInfo.pcStart, fdeInfo.pcEnd, fdeInfo.fdeStart);
#endif
			}
			return true;
		}
	}
	//DEBUG_MESSAGE("can't find/use FDE for pc=0x%llX\n", (uint64_t)pc);
	return false;
}


template <typename A, typename R, typename R2>
bool UnwindCursor<A,R,R2>::getInfoFromCompactEncodingSection(pint_t pc, pint_t mh, pint_t unwindSectionStart)
{	
	const bool log = false;
	if ( log ) fprintf(stderr, "getInfoFromCompactEncodingSection(pc=0x%llX, mh=0x%llX)\n", (uint64_t)pc, (uint64_t)mh);
	
	const UnwindSectionHeader<A> sectionHeader(fAddressSpace, unwindSectionStart);
	if ( sectionHeader.version() != UNWIND_SECTION_VERSION )
		return false;
	
	// do a binary search of top level index to find page with unwind info
	uint32_t targetFunctionOffset = pc - mh;
	const UnwindSectionIndexArray<A> topIndex(fAddressSpace, unwindSectionStart + sectionHeader.indexSectionOffset());
	uint32_t low = 0;
	uint32_t high = sectionHeader.indexCount();
	const uint32_t last = high - 1;
	while ( low < high ) {
		uint32_t mid = (low + high)/2;
		//if ( log ) fprintf(stderr, "\tmid=%d, low=%d, high=%d, *mid=0x%08X\n", mid, low, high, topIndex.functionOffset(mid));
		if ( topIndex.functionOffset(mid) <= targetFunctionOffset ) {
			if ( (mid == last) || (topIndex.functionOffset(mid+1) > targetFunctionOffset) ) {
				low = mid;
				break;
			}
			else {
				low = mid+1;
			}
		}
		else {
			high = mid;
		}
	}
	const uint32_t firstLevelFunctionOffset = topIndex.functionOffset(low);
	const uint32_t firstLevelNextPageFunctionOffset = topIndex.functionOffset(low+1);
	const pint_t secondLevelAddr    = unwindSectionStart+topIndex.secondLevelPagesSectionOffset(low);
	const pint_t lsdaArrayStartAddr = unwindSectionStart+topIndex.lsdaIndexArraySectionOffset(low);
	const pint_t lsdaArrayEndAddr   = unwindSectionStart+topIndex.lsdaIndexArraySectionOffset(low+1);
	if ( log ) fprintf(stderr, "\tfirst level search for result index=%d to secondLevelAddr=0x%llX\n", 
			low, (uint64_t)secondLevelAddr);
	// do a binary search of second level page index
	uint32_t encoding = 0;
	pint_t funcStart = 0;
	pint_t funcEnd = 0;
	pint_t lsda = 0;
	pint_t personality = 0;
	uint32_t pageKind = fAddressSpace.get32(secondLevelAddr);
	if ( pageKind == UNWIND_SECOND_LEVEL_REGULAR ) {
		// regular page
		UnwindSectionRegularPageHeader<A> pageHeader(fAddressSpace, secondLevelAddr);
		UnwindSectionRegularArray<A> pageIndex(fAddressSpace, secondLevelAddr + pageHeader.entryPageOffset());
		// binary search looks for entry with e where index[e].offset <= pc < index[e+1].offset
		if ( log ) fprintf(stderr, "\tbinary search for targetFunctionOffset=0x%08llX in regular page starting at secondLevelAddr=0x%llX\n", 
			(uint64_t)targetFunctionOffset, (uint64_t)secondLevelAddr);
		uint32_t low = 0;
		uint32_t high = pageHeader.entryCount();
		while ( low < high ) {
			uint32_t mid = (low + high)/2;
			if ( pageIndex.functionOffset(mid) <= targetFunctionOffset ) {
				if ( mid == (uint32_t)(pageHeader.entryCount()-1) ) {
					// at end of table
					low = mid;
					funcEnd = firstLevelNextPageFunctionOffset + mh;
					break;
				}
				else if ( pageIndex.functionOffset(mid+1) > targetFunctionOffset ) {
					// next is too big, so we found it
					low = mid;
					funcEnd = pageIndex.functionOffset(low+1) + mh;
					break;
				}
				else {
					low = mid+1;
				}
			}
			else {
				high = mid;
			}
		}
		encoding  = pageIndex.encoding(low);
		funcStart = pageIndex.functionOffset(low) + mh;
		if ( pc < funcStart  ) {
			if ( log ) fprintf(stderr, "\tpc not in table, pc=0x%llX, funcStart=0x%llX, funcEnd=0x%llX\n", (uint64_t)pc, (uint64_t)funcStart, (uint64_t)funcEnd);
			return false;
		}
		if ( pc > funcEnd ) {
			if ( log ) fprintf(stderr, "\tpc not in table, pc=0x%llX, funcStart=0x%llX, funcEnd=0x%llX\n", (uint64_t)pc, (uint64_t)funcStart, (uint64_t)funcEnd);
			return false;
		}
	}
	else if ( pageKind == UNWIND_SECOND_LEVEL_COMPRESSED ) {
		// compressed page
		UnwindSectionCompressedPageHeader<A> pageHeader(fAddressSpace, secondLevelAddr);
		UnwindSectionCompressedArray<A> pageIndex(fAddressSpace, secondLevelAddr + pageHeader.entryPageOffset());
		const uint32_t targetFunctionPageOffset = targetFunctionOffset - firstLevelFunctionOffset;
		// binary search looks for entry with e where index[e].offset <= pc < index[e+1].offset
		if ( log ) fprintf(stderr, "\tbinary search of compressed page starting at secondLevelAddr=0x%llX\n", (uint64_t)secondLevelAddr);
		uint32_t low = 0;
		const uint32_t last = pageHeader.entryCount() - 1;
		uint32_t high = pageHeader.entryCount();
		while ( low < high ) {
			uint32_t mid = (low + high)/2;
			if ( pageIndex.functionOffset(mid) <= targetFunctionPageOffset ) {
				if ( (mid == last) || (pageIndex.functionOffset(mid+1) > targetFunctionPageOffset) ) {
					low = mid;
					break;
				}
				else {
					low = mid+1;
				}
			}
			else {
				high = mid;
			}
		}
		funcStart = pageIndex.functionOffset(low) + firstLevelFunctionOffset + mh;
		if ( low < last )
			funcEnd = pageIndex.functionOffset(low+1) + firstLevelFunctionOffset + mh;
		else
			funcEnd = firstLevelNextPageFunctionOffset + mh;
		if ( pc < funcStart  ) {
			DEBUG_MESSAGE("malformed __unwind_info, pc=0x%llX not in second level compressed unwind table. funcStart=0x%llX\n", (uint64_t)pc, (uint64_t)funcStart);
			return false;
		}
		if ( pc > funcEnd ) {
			DEBUG_MESSAGE("malformed __unwind_info, pc=0x%llX not in second level compressed unwind table. funcEnd=0x%llX\n", (uint64_t)pc, (uint64_t)funcEnd);
			return false;
		}
		uint16_t encodingIndex = pageIndex.encodingIndex(low);
		if ( encodingIndex < sectionHeader.commonEncodingsArrayCount() ) {
			// encoding is in common table in section header
			encoding = fAddressSpace.get32(unwindSectionStart+sectionHeader.commonEncodingsArraySectionOffset()+encodingIndex*sizeof(uint32_t));
		}
		else {
			// encoding is in page specific table
			uint16_t pageEncodingIndex = encodingIndex-sectionHeader.commonEncodingsArrayCount();
			encoding = fAddressSpace.get32(secondLevelAddr+pageHeader.encodingsPageOffset()+pageEncodingIndex*sizeof(uint32_t));
		}
	}
	else {
		DEBUG_MESSAGE("malformed __unwind_info at 0x%0llX bad second level page\n", (uint64_t)unwindSectionStart);
		return false;
	}

	// look up LSDA, if encoding says function has one
	if ( encoding & UNWIND_HAS_LSDA ) {
		UnwindSectionLsdaArray<A>  lsdaIndex(fAddressSpace, lsdaArrayStartAddr);
		uint32_t funcStartOffset = funcStart - mh;
		uint32_t low = 0;
		uint32_t high = (lsdaArrayEndAddr-lsdaArrayStartAddr)/sizeof(unwind_info_section_header_lsda_index_entry);
		// binary search looks for entry with exact match for functionOffset
		if ( log ) fprintf(stderr, "\tbinary search of lsda table for targetFunctionOffset=0x%08X\n", funcStartOffset);
		while ( low < high ) {
			uint32_t mid = (low + high)/2;
			if ( lsdaIndex.functionOffset(mid) == funcStartOffset ) {
				lsda = lsdaIndex.lsdaOffset(mid) + mh;
				break;
			}
			else if ( lsdaIndex.functionOffset(mid) < funcStartOffset ) {
				low = mid+1;
			}
			else {
				high = mid;
			}
		}
		if ( lsda == 0 ) {
			DEBUG_MESSAGE("found encoding 0x%08X with HAS_LSDA bit set for pc=0x%0llX, but lsda table has no entry\n", encoding, (uint64_t)pc);
			return false;
		}
	}

	// extact personality routine, if encoding says function has one
	uint32_t personalityIndex = (encoding & UNWIND_PERSONALITY_MASK) >> (__builtin_ctz(UNWIND_PERSONALITY_MASK));	
	if ( personalityIndex != 0 ) {
		--personalityIndex; // change 1-based to zero-based index
		if ( personalityIndex > sectionHeader.personalityArrayCount() ) {
			DEBUG_MESSAGE("found encoding 0x%08X with personality index %d, but personality table has only %d entires\n", 
							encoding, personalityIndex, sectionHeader.personalityArrayCount());
			return false;
		}
		int32_t personalityDelta = fAddressSpace.get32(unwindSectionStart+sectionHeader.personalityArraySectionOffset()+personalityIndex*sizeof(uint32_t));
		pint_t personalityPointer = personalityDelta + mh;
		personality = fAddressSpace.getP(personalityPointer);
		if (log ) fprintf(stderr, "getInfoFromCompactEncodingSection(pc=0x%llX), personalityDelta=0x%08X, personality=0x%08llX\n", 
			(uint64_t)pc, personalityDelta, (uint64_t)personality);
	}
	
	if (log ) fprintf(stderr, "getInfoFromCompactEncodingSection(pc=0x%llX), encoding=0x%08X, lsda=0x%08llX for funcStart=0x%llX\n", 
						(uint64_t)pc, encoding, (uint64_t)lsda, (uint64_t)funcStart);
	fInfo.start_ip			= funcStart; 
	fInfo.end_ip			= funcEnd;
	fInfo.lsda				= lsda; 
	fInfo.handler			= personality;
	fInfo.gp				= 0;
	fInfo.flags				= 0;
	fInfo.format			= encoding;
	fInfo.unwind_info		= 0;
	fInfo.unwind_info_size	= 0;
	fInfo.extra				= mh;
	return true;
}



template <typename A, typename R, typename R2>
void UnwindCursor<A,R,R2>::setInfoBasedOnIPRegister(bool isReturnAddress)
{
	pint_t pc = this->getReg(UNW_REG_IP);
	switchArchIfNecessary(pc);
	// if the last line of a function is a "throw" the compile sometimes
	// emits no instructions after the call to __cxa_throw.  This means 
	// the return address is actually the start of the next function.
	// To disambiguate this, back up the pc when we know it is a return
	// address.
    if ( isReturnAddress )
        --pc;
    
    
	// ask address space object to find unwind sections for this pc
	pint_t mh;
	pint_t dwarfStart;
	pint_t dwarfLength;
	pint_t compactStart;
	if ( fAddressSpace.findUnwindSections(pc, mh, dwarfStart, dwarfLength, compactStart) ) {
		// if there is a compact unwind encoding table, look there first
		if ( compactStart != 0 ) {
			if ( this->getInfoFromCompactEncodingSection(pc, mh, compactStart) ) {
#if !FOR_DYLD
				// found info in table, done unless encoding says to use dwarf
				uint32_t offsetInDwarfSection;
				if ( (dwarfStart != 0) && dwarfWithOffset(offsetInDwarfSection) ) {
					if ( this->getInfoFromDwarfSection(pc, mh, dwarfStart, dwarfLength, offsetInDwarfSection) ) {
						// found info in dwarf, done
						return;
					}
				}
#endif
				// if unwind table has entry, but entry says there is no unwind info, note that
				if ( fInfo.format == 0 )
					fUnwindInfoMissing = true;

				// old compact encoding 
				if ( !mustUseDwarf() ) {
					return;
				}	
			}
		}
#if !FOR_DYLD || __ppc__
		// if there is dwarf unwind info, look there next
		if ( dwarfStart != 0 ) {
			if ( this->getInfoFromDwarfSection(pc, mh, dwarfStart, dwarfLength, 0) ) {
				// found info in dwarf, done
				return;
			}
		}
#endif
	}
	
#if !FOR_DYLD 
	// the PC is not in code loaded by dyld, look through __register_frame() registered FDEs
	pint_t cachedFDE = DwarfFDECache<A>::findFDE(0, pc);
	if ( cachedFDE != 0 ) {
		CFI_Parser<LocalAddressSpace>::FDE_Info fdeInfo;
		CFI_Parser<LocalAddressSpace>::CIE_Info cieInfo;
		const char* msg = CFI_Parser<A>::decodeFDE(fAddressSpace, cachedFDE, &fdeInfo, &cieInfo);
		if ( msg == NULL ) {
			typename CFI_Parser<A>::PrologInfo prolog;
			if ( CFI_Parser<A>::parseFDEInstructions(fAddressSpace, fdeInfo, cieInfo, pc, &prolog) ) {
				// save off parsed FDE info
				fInfo.start_ip			= fdeInfo.pcStart;
				fInfo.end_ip			= fdeInfo.pcEnd;
				fInfo.lsda				= fdeInfo.lsda;
				fInfo.handler			= cieInfo.personality;
				fInfo.gp				= prolog.spExtraArgSize;  // some frameless functions need SP altered when resuming in function
				fInfo.flags				= 0;
				fInfo.format			= dwarfEncoding();  
				fInfo.unwind_info		= fdeInfo.fdeStart;
				fInfo.unwind_info_size	= fdeInfo.fdeLength;
				fInfo.extra				= 0;
				return;
			}
		}
	}
	
#if KEYMGR_SUPPPORT  
	// lastly check for old style keymgr registration of dynamically generated FDEs
	// acquire exclusive access to libgcc_object_info
	libgcc_object_info* head = (libgcc_object_info*)_keymgr_get_and_lock_processwide_ptr(KEYMGR_GCC3_DW2_OBJ_LIST);
	if ( head != NULL ) {
		// look at each FDE in keymgr
		for (libgcc_object* ob = head->unseen_objects; ob != NULL; ob = ob->next) {
			CFI_Parser<LocalAddressSpace>::FDE_Info fdeInfo;
			CFI_Parser<LocalAddressSpace>::CIE_Info cieInfo;
			const char* msg = CFI_Parser<A>::decodeFDE(fAddressSpace, (pint_t)ob->fde, &fdeInfo, &cieInfo);
			if ( msg == NULL ) {
				// see if this FDE is for a function that includes the pc we are looking for
				if ( (fdeInfo.pcStart <= pc) && (pc < fdeInfo.pcEnd) ) {
					typename CFI_Parser<A>::PrologInfo prolog;
					if ( CFI_Parser<A>::parseFDEInstructions(fAddressSpace, fdeInfo, cieInfo, pc, &prolog) ) {
						// save off parsed FDE info
						fInfo.start_ip			= fdeInfo.pcStart;
						fInfo.end_ip			= fdeInfo.pcEnd;
						fInfo.lsda				= fdeInfo.lsda;
						fInfo.handler			= cieInfo.personality;
						fInfo.gp				= prolog.spExtraArgSize;  // some frameless functions need SP altered when resuming in function
						fInfo.flags				= 0;
						fInfo.format			= dwarfEncoding();  
						fInfo.unwind_info		= fdeInfo.fdeStart;
						fInfo.unwind_info_size	= fdeInfo.fdeLength;
						fInfo.extra				= 0;
						_keymgr_set_and_unlock_processwide_ptr(KEYMGR_GCC3_DW2_OBJ_LIST, head);
						return;
					}
				}
			}
		}
	}
	// release libgcc_object_info 
	_keymgr_set_and_unlock_processwide_ptr(KEYMGR_GCC3_DW2_OBJ_LIST, head);
#endif // KEYMGR_SUPPPORT
	
#endif

	// no unwind info, flag that we can't reliable unwind
	fUnwindInfoMissing = true;
}

template <typename A, typename R, typename R2>
int UnwindCursor<A,R,R2>::switchArchIfNecessary(pint_t &pc)
{
    // We assume that Regset_No_1 is the native arch, and
    // Regset_No_2 is the emulated one.
    if(!is_iqemu_available())
        return 0;
    
    switch(fRegSetNo) {
    case Regset_No_1:
        if(iqemu_is_code_in_jit(pc)) {
            fRegSetNo = Regset_No_2;
            
            uintptr_t new_sp, new_fp, new_pc;
            
            if(!iqemu_get_arm64_CFI(fJmpBufIndex, &new_sp, &new_fp, &new_pc)) {
                abort();
            }

            pc = new_pc;
            fRegisters2.setIP(pc);
            fRegisters2.setSP(new_sp);
            fRegisters2.setFP(new_fp);
            
            setJmpBuf(iqemu_get_jmp_buf(fJmpBufIndex));
        }
        
        break;
    case Regset_No_2:
        if(!iqemu_need_emulation(pc)) {
            fRegSetNo = Regset_No_1;
            
            uintptr_t new_sp, new_fp;
            
            if(!iqemu_get_x64_CFI(fJmpBufIndex++, &new_sp, &new_fp)) {
                // XXX: Maybe some way better than abort?
                abort();
            }
            
            fRegisters.setSP(new_sp);
            fRegisters.setRBP(new_fp);
        }
        
        break;
    default:
        abort();
    }
    
    return 0;
}

template <typename A, typename R, typename R2>
int UnwindCursor<A,R,R2>::step()
{
	// bottom of stack is defined as when no more unwind info
	if ( fUnwindInfoMissing )
		return UNW_STEP_END;

	// apply unwinding to register set
	int result;
	if ( this->mustUseDwarf() )
		result = this->stepWithDwarfFDE();
	else
		result = this->stepWithCompactEncoding();
	
	// update info based on new PC
	if ( result == UNW_STEP_SUCCESS ) {
		this->setInfoBasedOnIPRegister(true);
		if ( fUnwindInfoMissing )
			return UNW_STEP_END;
	}

	return result;
}


template <typename A, typename R, typename R2>
void UnwindCursor<A,R,R2>::getInfo(unw_proc_info_t* info)
{
	*info = fInfo;
}


template <typename A, typename R, typename R2>
bool UnwindCursor<A,R,R2>::getFunctionName(char* buf, size_t bufLen, unw_word_t* offset)
{
	return fAddressSpace.findFunctionName(this->getReg(UNW_REG_IP), buf, bufLen, offset);
}

}; // namespace libunwind 


#endif // __UNWINDCURSOR_HPP__




