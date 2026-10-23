// Ghidra pre-script to configure reproducible analysis options for re5-recompiled.
// @category Analysis

import ghidra.app.script.GhidraScript;

public class ConfigureAnalysis extends GhidraScript {
    @Override
    public void run() throws Exception {
        println("=== Configuring Analysis Options for RE5 ===");

        // Analisadores ligados:
        // RTTI e EH da Microsoft
        setAnalysisOption(currentProgram, "Windows x86 PE RTTI Analyzer", "true");
        setAnalysisOption(currentProgram, "Windows x86 PE Exception Handling", "true");

        // FunctionID
        setAnalysisOption(currentProgram, "Function ID", "true");

        // Tabelas de salto do decompilador
        setAnalysisOption(currentProgram, "Decompiler Switch Analysis", "true");

        // Funções sem retorno
        setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "true");
        setAnalysisOption(currentProgram, "Non-Returning Functions - Known", "true");

        // Demangler
        setAnalysisOption(currentProgram, "Demangler Microsoft", "true");

        // Tipos do Windows
        setAnalysisOption(currentProgram, "Apply Data Archives", "true");
        setAnalysisOption(currentProgram, "Windows x86 Thread Environment Block (TEB) Analyzer", "true");

        // Desligados na 1ª passada (lentos e cheios de falsos positivos)
        setAnalysisOption(currentProgram, "Aggressive Instruction Finder", "false");
        setAnalysisOption(currentProgram, "Decompiler Parameter ID", "false");

        println("Analysis options configured successfully.");
    }
}
