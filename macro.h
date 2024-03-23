#ifndef MACRO_H_
#define MACRO_H_

#define TRUE  1
#define FALSE 0

#define MAX_CONT (10*1024*1024)  /* maximum content length */
#define MAX_HDR  (1024)          /* maximum header length */

/* debug trace */
#ifndef OFFTRACE
#define TRACE(fmt, msg...) \
    fprintf(stderr, "%s (%s:%d) " fmt, __FUNCTION__, __FILE__, __LINE__, ##msg)
#else    
    #define TRACE(fmt, msg...) (void)0
#endif

#endif