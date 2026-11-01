// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
// unit_com_try.cpp
//
// COM_TRY_BEGIN / COM_TRY_END (CPP/Common/ComTry.h): solo i fallimenti di allocazione
// diventano E_OUTOFMEMORY; le altre eccezioni (errori interni, dati malformati segnalati
// con "throw <numero>") diventano E_FAIL. Upstream restituiva E_OUTOFMEMORY per tutte.

#include "../CPP/Common/Common.h"

#include <stdio.h>

#include <new>

#include "../CPP/Common/MyWindows.h"
#include "../CPP/Common/MyInitGuid.h"

#include "../CPP/Common/ComTry.h"
#include "../CPP/Common/MyString.h"
#include "../CPP/Common/MyVector.h"
#include "../CPP/7zip/UI/Common/IFileExtractCallback.h" // definisce i GUID usati dagli oggetti linkati

static unsigned g_NumChecks = 0;
static unsigned g_NumFailures = 0;

#define CHECK(cond) \
  do { g_NumChecks++; if (!(cond)) { g_NumFailures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static HRESULT Ok()          { COM_TRY_BEGIN return S_OK; COM_TRY_END }
static HRESULT ThrowInt()    { COM_TRY_BEGIN throw 20120424; COM_TRY_END }
static HRESULT ThrowNew()    { COM_TRY_BEGIN throw CNewException(); COM_TRY_END }
static HRESULT ThrowStdBad() { COM_TRY_BEGIN throw std::bad_alloc(); COM_TRY_END }
static HRESULT ThrowString() { COM_TRY_BEGIN throw UString("message"); COM_TRY_END }

// allocations that can't be done: the containers throw CNewException
static HRESULT VectorTooBig()
{
  COM_TRY_BEGIN
  CRecordVector<UInt64> v;
  v.Reserve(0xFFFFFFFF);
  return S_OK;
  COM_TRY_END
}

int main()
{
  CHECK(Ok() == S_OK);
  CHECK(ThrowInt() == E_FAIL);
  CHECK(ThrowString() == E_FAIL);
  CHECK(ThrowNew() == E_OUTOFMEMORY);
  CHECK(ThrowStdBad() == E_OUTOFMEMORY);
  CHECK(VectorTooBig() == E_OUTOFMEMORY);
  printf("%u controlli, %u falliti\n", g_NumChecks, g_NumFailures);
  return g_NumFailures == 0 ? 0 : 1;
}
