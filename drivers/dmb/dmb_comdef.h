#ifndef _DMB_COMDEF_H_
#define _DMB_COMDEF_H_
#define DMB_KERN_MSG_ALERT(fmt, args...) pr_debug(fmt, ##args)
#define DMB_KERN_MSG_LIMIT_CRIT(fmt, args...) pr_debug(fmt, ##args)
#endif
