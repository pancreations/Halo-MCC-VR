// Keep H4EK headless analysis focused on code, functions, and references.
// The default non-return inference spent the entire first 30-minute pass.
// @category HaloMCCVR.RE

import ghidra.app.script.GhidraScript;

public class ConfigureH4KitAnalysis extends GhidraScript {
    @Override
    public void run() throws Exception {
        setAnalysisOption(currentProgram,
            "Non-Returning Functions - Discovered", "false");
        setAnalysisOption(currentProgram, "Call Convention ID", "false");
        setAnalysisOption(currentProgram, "Decompiler Parameter ID", "false");
        setAnalysisOption(currentProgram, "Function ID", "false");
        setAnalysisOption(currentProgram, "Stack", "false");
        setAnalysisOption(currentProgram,
            "Windows x86 PE RTTI Analyzer", "false");
        println("Configured focused H4EK code/reference analysis");
    }
}
