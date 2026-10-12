/* tests/dummy_crash.c — B-01 取证装置自测 dummy（原生 AV 崩溃进程）
 *
 * 触发真正的未处理 access violation（写 NULL 地址），进程以未处理异常终止，
 * 退出码 0xC0000005。用于验证 tests/forward_forensics.py：
 *   - 直接模式：EXIT-CODE 3221225477 (0xC0000005) / EXIT-CLASS CRASH: ACCESS_VIOLATION
 *   - --cdb 模式：second-chance 自动 .dump 落盘 + kb 调用栈
 *
 * 编译（产物放任务 logs/，供 logs/fix-b01-selftest.ps1 使用）：
 *   cmd /c ""C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" && cl /nologo /O1 /Fe <logs>\dummy_crash.exe tests\dummy_crash.c"
 *
 * 背景：Python 3.14 解释器内无法产生未处理 AV（ctypes 调用链有 SEH 保护转
 * Python 异常→退出码 1；faulthandler 默认接管 _sigsegv()→abort() 退出码 3），
 * 故用原生 C dummy。不涉及任何产品代码。
 */
int main(void)
{
    volatile int *p = 0;
    *p = 1; /* 写 NULL → 未处理 AV → 进程退出码 0xC0000005 */
    return 0;
}
