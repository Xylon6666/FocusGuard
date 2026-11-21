/* 取证用探针：照抄 FocusGuard 的两次 SetThreadExecutionState 调用，
   打印每次调用的返回值（返回值 = 调用前的该线程执行状态）。
   编译：cl /nologo /O2 probe_es.c                                        */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    DWORD p0 = SetThreadExecutionState(ES_CONTINUOUS);                 /* 复位 */
    DWORD p1 = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED); /* 打开 */
    DWORD p2 = SetThreadExecutionState(ES_CONTINUOUS);                 /* 关闭 */
    printf("prev_before_reset = 0x%08X\n", p0);
    printf("prev_before_on    = 0x%08X  (ES_CONTINUOUS=0x%08X)\n", p1, ES_CONTINUOUS);
    printf("prev_after_on     = 0x%08X  (连续+系统+显示 全置位 = %s)\n", p2,
           (p2 & (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)) ==
           (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) ? "是" : "否");
    return 0;
}
