// Ghidra script to export all V1 audit datasets for re5-recompiled
// Outputs: switches.json, code_ptrs.json, imports.json, outside.json,
//          fs_refs.json, text_writes.json, eh.json, cpuid.json, strings.json, instr.json
// @category Export

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.File;
import java.io.PrintWriter;
import java.util.*;

public class ExportAllV1 extends GhidraScript {
    private File outDir;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        outDir = (args != null && args.length > 0) ? new File(args[0]) : new File("work/ghidra/export");
        outDir.mkdirs();

        println("=== Starting V1 Dataset Export to " + outDir.getAbsolutePath() + " ===");

        exportSwitches();
        exportCodePtrs();
        exportImports();
        exportOutsideAndFs();
        exportCpuidAndEh();
        exportStrings();
        exportInstrCensus();

        println("=== V1 Dataset Export Complete ===");
    }

    private String hex(long v) {
        return String.format("0x%08x", v);
    }

    private String esc(String s) {
        if (s == null) return "";
        return s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n").replace("\r", "");
    }

    // 1. switches.json: computed jumps and calls
    private void exportSwitches() throws Exception {
        File file = new File(outDir, "switches.json");
        println("Exporting switches to " + file.getName() + "...");
        PrintWriter w = new PrintWriter(file, "UTF-8");
        w.println("{");

        Listing listing = currentProgram.getListing();
        InstructionIterator it = listing.getInstructions(true);
        boolean first = true;
        int count = 0;

        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            FlowType flow = ins.getFlowType();
            if (flow.isJump() && flow.isComputed()) {
                Address addr = ins.getAddress();
                Function f = listing.getFunctionContaining(addr);
                Reference[] refs = ins.getReferencesFrom();
                List<String> targets = new ArrayList<>();
                Address tableAddr = null;

                for (Reference r : refs) {
                    if (r.getReferenceType().isFlow()) {
                        targets.add(hex(r.getToAddress().getOffset()));
                    } else if (r.getReferenceType().isData()) {
                        tableAddr = r.getToAddress();
                    }
                }

                if (!first) w.println(",");
                first = false;

                w.print("  \"" + hex(addr.getOffset()) + "\": {");
                w.print("\"addr\": \"" + hex(addr.getOffset()) + "\", ");
                w.print("\"func\": \"" + (f != null ? hex(f.getEntryPoint().getOffset()) : "") + "\", ");
                w.print("\"mnemonic\": \"" + ins.getMnemonicString() + "\", ");
                w.print("\"table\": \"" + (tableAddr != null ? hex(tableAddr.getOffset()) : "") + "\", ");
                w.print("\"case_count\": " + targets.size() + ", ");
                w.print("\"targets\": [");
                for (int i = 0; i < targets.size(); i++) {
                    if (i > 0) w.print(", ");
                    w.print("\"" + targets.get(i) + "\"");
                }
                w.print("]}");
                count++;
            }
        }
        w.println();
        w.println("}");
        w.close();
        println("  Exported " + count + " computed switches.");
    }

    // 2. code_ptrs.json: pointers in data sections targeting code
    private void exportCodePtrs() throws Exception {
        File file = new File(outDir, "code_ptrs.json");
        println("Exporting code pointers to " + file.getName() + "...");
        PrintWriter w = new PrintWriter(file, "UTF-8");
        w.println("[");

        Memory mem = currentProgram.getMemory();
        Listing listing = currentProgram.getListing();
        MemoryBlock textBlock = mem.getBlock(".text");
        long textStart = textBlock != null ? textBlock.getStart().getOffset() : 0x00401000L;
        long textEnd = textBlock != null ? textBlock.getEnd().getOffset() : 0x00f4a000L;

        boolean first = true;
        int count = 0;

        for (MemoryBlock block : mem.getBlocks()) {
            if (block.isExecute() || !block.isInitialized()) continue;
            String secName = block.getName();
            Address start = block.getStart();
            long size = block.getSize();

            byte[] buf = new byte[4096];
            for (long offset = 0; offset + 4 <= size; offset += 4) {
                if (monitor.isCancelled()) break;
                Address curr = start.add(offset);
                try {
                    int val = mem.getInt(curr);
                    long target = val & 0xffffffffL;
                    if (target >= textStart && target <= textEnd) {
                        Function targetFn = listing.getFunctionAt(curr.getNewAddress(target));
                        boolean isFuncEntry = (targetFn != null);

                        if (!first) w.println(",");
                        first = false;

                        w.print("  {\"origin\": \"" + hex(curr.getOffset()) + "\", ");
                        w.print("\"target\": \"" + hex(target) + "\", ");
                        w.print("\"section\": \"" + esc(secName) + "\", ");
                        w.print("\"is_entry\": " + isFuncEntry + "}");
                        count++;
                    }
                } catch (Exception ignored) {}
            }
        }
        w.println();
        w.println("]");
        w.close();
        println("  Exported " + count + " code pointers from data.");
    }

    // 3. imports.json: callers of every imported function
    private void exportImports() throws Exception {
        File file = new File(outDir, "imports.json");
        println("Exporting imports to " + file.getName() + "...");
        PrintWriter w = new PrintWriter(file, "UTF-8");
        w.println("{");

        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        SymbolTable st = currentProgram.getSymbolTable();

        SymbolIterator symIt = st.getExternalSymbols();
        boolean first = true;
        int count = 0;

        while (symIt.hasNext() && !monitor.isCancelled()) {
            Symbol sym = symIt.next();
            String name = sym.getName();
            String parent = sym.getParentNamespace().getName();
            String fullImport = parent + "!" + name;

            ReferenceIterator refIt = rm.getReferencesTo(sym.getAddress());
            List<String> callers = new ArrayList<>();
            List<Boolean> addrTaken = new ArrayList<>();

            while (refIt.hasNext()) {
                Reference r = refIt.next();
                Address from = r.getFromAddress();
                Instruction ins = listing.getInstructionAt(from);
                boolean isCall = (ins != null && ins.getFlowType().isCall());
                callers.add(hex(from.getOffset()));
                addrTaken.add(!isCall);
            }

            if (!first) w.println(",");
            first = false;

            w.print("  \"" + esc(fullImport) + "\": {");
            w.print("\"dll\": \"" + esc(parent) + "\", ");
            w.print("\"name\": \"" + esc(name) + "\", ");
            w.print("\"callers\": [");
            for (int i = 0; i < callers.size(); i++) {
                if (i > 0) w.print(", ");
                w.print("{\"addr\": \"" + callers.get(i) + "\", \"addr_taken\": " + addrTaken.get(i) + "}");
            }
            w.print("]}");
            count++;
        }
        w.println();
        w.println("}");
        w.close();
        println("  Exported " + count + " import entries.");
    }

    // 4. outside.json & fs_refs.json & text_writes.json
    private void exportOutsideAndFs() throws Exception {
        File fOutside = new File(outDir, "outside.json");
        File fFs = new File(outDir, "fs_refs.json");
        File fWrites = new File(outDir, "text_writes.json");

        PrintWriter wOut = new PrintWriter(fOutside, "UTF-8");
        PrintWriter wFs = new PrintWriter(fFs, "UTF-8");
        PrintWriter wWr = new PrintWriter(fWrites, "UTF-8");

        wOut.println("[");
        wFs.println("{");
        wWr.println("[");

        Listing listing = currentProgram.getListing();
        InstructionIterator it = listing.getInstructions(true);
        Memory mem = currentProgram.getMemory();

        Map<String, Integer> fsCounts = new HashMap<>();
        boolean firstOut = true, firstWr = true;
        int outCount = 0, wrCount = 0;

        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            Address addr = ins.getAddress();
            String str = ins.toString().toUpperCase();

            // FS segment check
            if (str.contains("FS:[") || str.contains("FS:")) {
                int idx = str.indexOf("FS:[");
                if (idx != -1) {
                    int endIdx = str.indexOf("]", idx);
                    if (endIdx != -1) {
                        String offsetStr = str.substring(idx + 4, endIdx).trim();
                        fsCounts.put(offsetStr, fsCounts.getOrDefault(offsetStr, 0) + 1);
                    }
                }
            }

            // References check: writes and outside memory
            for (Reference r : ins.getReferencesFrom()) {
                Address to = r.getToAddress();
                if (to == null || !to.isMemoryAddress()) continue;
                long offset = to.getOffset();

                // Outside check (protect zone 0x0137F000..0x01CFF000 or KUSER 0x7FFE0000 or < 0x400000)
                if (offset >= 0x0137F000L && offset <= 0x01CFF000L || offset >= 0x7ffe0000L && offset <= 0x7ffe1000L) {
                    if (!firstOut) wOut.println(",");
                    firstOut = false;
                    wOut.print("  {\"ins\": \"" + hex(addr.getOffset()) + "\", \"target\": \"" + hex(offset) + "\", \"kind\": \"outside_range\"}");
                    outCount++;
                }

                // Text write check
                if (r.getReferenceType().isWrite()) {
                    MemoryBlock b = mem.getBlock(to);
                    if (b != null && (b.getName().equals(".text") || b.getName().equals(".rdata"))) {
                        if (!firstWr) wWr.println(",");
                        firstWr = false;
                        wWr.print("  {\"ins\": \"" + hex(addr.getOffset()) + "\", \"target\": \"" + hex(offset) + "\", \"sec\": \"" + b.getName() + "\"}");
                        wrCount++;
                    }
                }
            }
        }

        wOut.println(); wOut.println("]"); wOut.close();
        wWr.println(); wWr.println("]"); wWr.close();

        // Write FS refs
        boolean firstFs = true;
        for (Map.Entry<String, Integer> e : fsCounts.entrySet()) {
            if (!firstFs) wFs.println(",");
            firstFs = false;
            wFs.print("  \"" + esc(e.getKey()) + "\": " + e.getValue());
        }
        wFs.println(); wFs.println("}"); wFs.close();

        println("  Exported outside.json (" + outCount + " entries), fs_refs.json (" + fsCounts.size() + " offsets), text_writes.json (" + wrCount + " writes).");
    }

    // 5. cpuid.json & eh.json
    private void exportCpuidAndEh() throws Exception {
        File fCpuid = new File(outDir, "cpuid.json");
        File fEh = new File(outDir, "eh.json");

        PrintWriter wCpuid = new PrintWriter(fCpuid, "UTF-8");
        PrintWriter wEh = new PrintWriter(fEh, "UTF-8");

        wCpuid.println("[");
        wEh.println("{");

        Listing listing = currentProgram.getListing();
        InstructionIterator it = listing.getInstructions(true);

        boolean firstCpuid = true;
        int cpuidCount = 0;
        int throwCallers = 0;

        List<String> throwList = new ArrayList<>();
        List<String> raiseList = new ArrayList<>();

        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            if (ins.getMnemonicString().equalsIgnoreCase("CPUID")) {
                Address addr = ins.getAddress();
                Function f = listing.getFunctionContaining(addr);
                Instruction prev = ins.getPrevious();

                if (!firstCpuid) wCpuid.println(",");
                firstCpuid = false;

                wCpuid.print("  {\"addr\": \"" + hex(addr.getOffset()) + "\", ");
                wCpuid.print("\"func\": \"" + (f != null ? hex(f.getEntryPoint().getOffset()) : "") + "\", ");
                wCpuid.print("\"prev\": \"" + (prev != null ? esc(prev.toString()) : "") + "\"}");
                cpuidCount++;
            }

            if (ins.getFlowType().isCall()) {
                for (Reference r : ins.getReferencesFrom()) {
                    Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(r.getToAddress());
                    if (s != null) {
                        String name = s.getName();
                        if (name.contains("CxxThrowException")) throwList.add(hex(ins.getAddress().getOffset()));
                        if (name.contains("RaiseException")) raiseList.add(hex(ins.getAddress().getOffset()));
                    }
                }
            }
        }

        wCpuid.println(); wCpuid.println("]"); wCpuid.close();

        wEh.println("  \"cxx_throw_callers\": [");
        for (int i = 0; i < throwList.size(); i++) {
            if (i > 0) wEh.print(", ");
            wEh.print("\"" + throwList.get(i) + "\"");
        }
        wEh.println("],");

        wEh.println("  \"raise_exception_callers\": [");
        for (int i = 0; i < raiseList.size(); i++) {
            if (i > 0) wEh.print(", ");
            wEh.print("\"" + raiseList.get(i) + "\"");
        }
        wEh.println("]");
        wEh.println("}");
        wEh.close();

        println("  Exported cpuid.json (" + cpuidCount + " sites), eh.json (" + throwList.size() + " throws, " + raiseList.size() + " raises).");
    }

    // 6. strings.json: defined strings with callers
    private void exportStrings() throws Exception {
        File file = new File(outDir, "strings.json");
        println("Exporting strings to " + file.getName() + "...");
        PrintWriter w = new PrintWriter(file, "UTF-8");
        w.println("{");

        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        DataIterator it = listing.getDefinedData(true);

        boolean first = true;
        int count = 0;

        while (it.hasNext() && !monitor.isCancelled()) {
            Data d = it.next();
            if (d.hasStringValue()) {
                String val = d.getDefaultValueRepresentation();
                if (val == null || val.length() < 3) continue;

                Address addr = d.getAddress();
                ReferenceIterator refIt = rm.getReferencesTo(addr);
                List<String> refs = new ArrayList<>();
                while (refIt.hasNext()) {
                    refs.add(hex(refIt.next().getFromAddress().getOffset()));
                }

                if (!first) w.println(",");
                first = false;

                w.print("  \"" + hex(addr.getOffset()) + "\": {");
                w.print("\"value\": \"" + esc(val) + "\", ");
                w.print("\"ref_count\": " + refs.size() + ", ");
                w.print("\"refs\": [");
                for (int i = 0; i < refs.size(); i++) {
                    if (i > 0) w.print(", ");
                    w.print("\"" + refs.get(i) + "\"");
                }
                w.print("]}");
                count++;
            }
        }

        w.println();
        w.println("}");
        w.close();
        println("  Exported " + count + " defined strings.");
    }

    // 7. instr.json: census of instruction mnemonics and vector/FPU use
    private void exportInstrCensus() throws Exception {
        File file = new File(outDir, "instr.json");
        println("Exporting instruction census to " + file.getName() + "...");
        PrintWriter w = new PrintWriter(file, "UTF-8");
        w.println("{");

        Listing listing = currentProgram.getListing();
        InstructionIterator it = listing.getInstructions(true);

        Map<String, Integer> mnemonics = new HashMap<>();
        int x87Count = 0, sseCount = 0, mmxCount = 0;

        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            String m = ins.getMnemonicString().toLowerCase();
            mnemonics.put(m, mnemonics.getOrDefault(m, 0) + 1);

            if (m.startsWith("f") && !m.equals("for") && !m.equals("fnop")) x87Count++;
            else if (m.endsWith("ps") || m.endsWith("ss") || m.startsWith("p") || m.startsWith("movd") || m.startsWith("movq")) {
                if (m.startsWith("mmx")) mmxCount++;
                else sseCount++;
            }
        }

        w.println("  \"totals\": {");
        w.println("    \"x87_instructions\": " + x87Count + ",");
        w.println("    \"sse_instructions\": " + sseCount + ",");
        w.println("    \"mmx_instructions\": " + mmxCount + "");
        w.println("  },");

        w.println("  \"mnemonics\": {");
        boolean first = true;
        List<Map.Entry<String, Integer>> list = new ArrayList<>(mnemonics.entrySet());
        list.sort((a, b) -> b.getValue().compareTo(a.getValue()));

        for (Map.Entry<String, Integer> e : list) {
            if (!first) w.println(",");
            first = false;
            w.print("    \"" + esc(e.getKey()) + "\": " + e.getValue());
        }
        w.println();
        w.println("  }");
        w.println("}");
        w.close();
        println("  Exported instruction census (" + mnemonics.size() + " unique mnemonics, " + x87Count + " x87, " + sseCount + " SSE).");
    }
}
