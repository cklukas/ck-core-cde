/* Test module for dtksh's loadable widget class interface. */
#include <ast.h>
#include <shcmd.h>
#include <Xm/PushB.h>

SHLIB(test_widget)

WidgetClass testWidgetClass;

void
lib_init(int mode, void *context)
{
    (void)mode;
    (void)context;
    testWidgetClass = xmPushButtonWidgetClass;
}
