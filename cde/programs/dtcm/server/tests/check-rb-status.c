/* Harness for check-rb-status.py: _DtCmsRbToCsaStat() must return a code
 * for every status, including one outside the enumeration. */
/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <stdio.h>
#include <assert.h>
typedef unsigned int CSA_return_code;
#define CSA_SUCCESS   ((CSA_return_code)0)
#define CSA_E_FAILURE ((CSA_return_code)6)
typedef enum {
	rb_ok		= 0,
	rb_duplicate	= 1,
	rb_badtable	= 2,
	rb_notable	= 3,
	rb_failed	= 4,
	rb_other	= 5
} Rb_Status;
/* @@BODY@@ */
int main(void)
{
    assert(_DtCmsRbToCsaStat(rb_ok) == CSA_SUCCESS);
    assert(_DtCmsRbToCsaStat(rb_duplicate) == CSA_E_FAILURE);
    assert(_DtCmsRbToCsaStat(rb_other) == CSA_E_FAILURE);
    assert(_DtCmsRbToCsaStat((Rb_Status) 42) == CSA_E_FAILURE);
    puts("rb status ok");
    return 0;
}
