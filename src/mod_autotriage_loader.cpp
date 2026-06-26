// AzerothCore module script loader.
// The function name must follow the convention:
//   Add + <module-dir-underscored> + Scripts
// so that AzerothCore's CMake toolchain can discover and invoke it.

void AddSC_AutoTriage();
void AddSC_AutoTriageCommands();
void AddSC_AutoTriagePeriodic();

void Addmod_autotriageScripts()
{
    AddSC_AutoTriage();
    AddSC_AutoTriageCommands();
    AddSC_AutoTriagePeriodic();
}
