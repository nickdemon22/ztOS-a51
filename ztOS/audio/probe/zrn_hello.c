// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
static int __init zrn_hello_init(void) { pr_info("zrn_hello: modules load on this kernel\n"); return 0; }
static void __exit zrn_hello_exit(void) { pr_info("zrn_hello: out\n"); }
module_init(zrn_hello_init);
module_exit(zrn_hello_exit);
MODULE_LICENSE("GPL");
