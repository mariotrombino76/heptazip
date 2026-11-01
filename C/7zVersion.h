#define MY_VER_MAJOR 26
#define MY_VER_MINOR 5
#define MY_VER_BUILD 0
#define MY_VERSION_NUMBERS "26.05"
#define MY_VERSION MY_VERSION_NUMBERS

/* Tensor7: the fork of 7-Zip (branch modern), version = upstream base + "-m" release */
#define T7_NAME     "Tensor7"
#define T7_RELEASE  1
#define T7_VERSION  MY_VERSION_NUMBERS "-m1"
#define T7_COPYRIGHT  "Copyright (c) 2026 Mario Trombino"

#ifdef MY_CPU_NAME
  #define MY_VERSION_CPU MY_VERSION " (" MY_CPU_NAME ")"
#else
  #define MY_VERSION_CPU MY_VERSION
#endif

#define MY_DATE "2026-10-21"
#undef MY_COPYRIGHT
#undef MY_VERSION_COPYRIGHT_DATE
#define MY_AUTHOR_NAME "Igor Pavlov"
#define MY_COPYRIGHT_PD "Igor Pavlov : Public domain"
#define MY_COPYRIGHT_CR "Copyright (c) 1999-2026 Igor Pavlov"

#ifdef USE_COPYRIGHT_CR
  #define MY_COPYRIGHT MY_COPYRIGHT_CR
#else
  #define MY_COPYRIGHT MY_COPYRIGHT_PD
#endif

#define MY_COPYRIGHT_DATE MY_COPYRIGHT " : " MY_DATE
#define MY_VERSION_COPYRIGHT_DATE MY_VERSION_CPU " : " MY_COPYRIGHT " : " MY_DATE
