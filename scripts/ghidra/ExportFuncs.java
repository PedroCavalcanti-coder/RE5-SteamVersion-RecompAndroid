// Ghidra script to export all functions to JSON for re5-recompiled (V1 funcs.json)
// @category Export

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.symbol.SourceType;
import java.io.File;
import java.io.PrintWriter;

public class ExportFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = (args != null && args.length > 0) ? new File(args[0]) : new File("work/ghidra/export");
        outDir.mkdirs();
        File outFile = new File(outDir, "funcs.json");

        println("Exporting functions to " + outFile.getAbsolutePath() + "...");
        PrintWriter writer = new PrintWriter(outFile, "UTF-8");
        writer.println("{");

        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionIterator it = fm.getFunctions(true);

        boolean first = true;
        int count = 0;

        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            Address entry = f.getEntryPoint();
            String addrHex = String.format("0x%08x", entry.getOffset());

            int callerCount = 0;
            ReferenceIterator refIt = rm.getReferencesTo(entry);
            while (refIt.hasNext()) {
                refIt.next();
                callerCount++;
            }

            String name = f.getName();
            SourceType srcType = f.getSymbol().getSource();
            boolean isThunk = f.isThunk();
            boolean noReturn = f.hasNoReturn();
            String cc = f.getCallingConventionName();
            if (cc == null) cc = "unknown";

            if (!first) {
                writer.println(",");
            }
            first = false;

            writer.print("  \"" + addrHex + "\": {");
            writer.print("\"entry\": \"" + addrHex + "\", ");
            writer.print("\"name\": \"" + escapeJson(name) + "\", ");
            writer.print("\"source\": \"" + srcType.name() + "\", ");
            writer.print("\"thunk\": " + isThunk + ", ");
            writer.print("\"no_return\": " + noReturn + ", ");
            writer.print("\"calling_conv\": \"" + escapeJson(cc) + "\", ");
            writer.print("\"callers\": " + callerCount + ", ");
            writer.print("\"size\": " + f.getBody().getNumAddresses() + ", ");

            writer.print("\"ranges\": [");
            boolean rFirst = true;
            for (AddressRange r : f.getBody().getAddressRanges()) {
                if (!rFirst) writer.print(", ");
                rFirst = false;
                writer.print("[\"0x" + Long.toHexString(r.getMinAddress().getOffset()) + "\", \"0x" + Long.toHexString(r.getMaxAddress().getOffset()) + "\"]");
            }
            writer.print("]");

            writer.print("}");
            count++;
        }

        writer.println();
        writer.println("}");
        writer.close();
        println("Exported " + count + " functions to " + outFile.getAbsolutePath());
    }

    private String escapeJson(String s) {
        if (s == null) return "";
        return s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n").replace("\r", "");
    }
}
