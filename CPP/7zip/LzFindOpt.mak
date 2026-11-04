# Heptazip: always the C version. It is faster than LzFindOpt.asm (LzFind_MatchEnd,
# design 0004), and since #77 GetMatchesSpecN_2() has more parameters (bt threads
# by hash bucket): the assembly version does not match it any more (it crashed the
# MSVC build of PR #78). Same as CMake (cmake/SevenZipAsm.cmake) and 7zip_gcc.mak.
C_OBJS = $(C_OBJS) \
  $O\LzFindOpt.obj
