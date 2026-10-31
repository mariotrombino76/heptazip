// ComTry.h

#ifndef ZIP7_INC_COM_TRY_H
#define ZIP7_INC_COM_TRY_H

#include "MyWindows.h"
#include "NewHandler.h"

/* 7zip-modern: only allocation failures (CNewException, std::bad_alloc) become
   E_OUTOFMEMORY. Upstream returned E_OUTOFMEMORY for any exception, so internal
   errors and malformed data reported with "throw <number>" were shown to the user
   as "not enough memory". The other exceptions now give E_FAIL.
   Where operator new() is redefined (CNewException is not std::bad_alloc), the
   standard library can still throw std::bad_alloc: both are caught. */
#ifdef Z7_REDEFINE_OPERATOR_NEW
  #include <new>
  #define Z7_COM_TRY_CATCH_STD_BAD_ALLOC  catch(const std::bad_alloc &) { return E_OUTOFMEMORY; }
#else
  #define Z7_COM_TRY_CATCH_STD_BAD_ALLOC
#endif

#define COM_TRY_BEGIN try {
#define COM_TRY_END } \
  catch(const CNewException &) { return E_OUTOFMEMORY; } \
  Z7_COM_TRY_CATCH_STD_BAD_ALLOC \
  catch(...) { return E_FAIL; }

#endif
