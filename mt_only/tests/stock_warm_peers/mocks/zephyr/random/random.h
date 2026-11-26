#pragma once
inline int sys_csrand_get(void *p,size_t n){static unsigned counter=0;memset(p,++counter,n);return 0;}
