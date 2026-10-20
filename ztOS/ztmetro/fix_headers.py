with open("ztmetro.c", "r") as f:
    code = f.read()

target = "#include <wlr/render/allocator.h>"
addition = """#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/util/log.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>"""

if target in code:
    code = code.replace(target, addition, 1)
    with open("ztmetro.c", "w") as f:
        f.write(code)
    print("SUCCESS: headers added")
else:
    print("ERROR: target not found")
