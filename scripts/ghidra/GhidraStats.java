// Ghidra headless script to extract analysis stats for re5-recompiled
// @category Analysis

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.Iterator;

public class GhidraStats extends GhidraScript {
    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        SymbolTable st = currentProgram.getSymbolTable();
        BookmarkManager bm = currentProgram.getBookmarkManager();

        int funcCount = fm.getFunctionCount();
        println("=== " + currentProgram.getName() + " Analysis Stats ===");
        println("Total functions found: " + funcCount);

        int named = 0;
        int unnamed = 0;
        int thunks = 0;
        int external = 0;
        FunctionIterator iter = fm.getFunctions(true);
        while (iter.hasNext()) {
            Function f = iter.next();
            if (f.isThunk()) thunks++;
            else if (f.isExternal()) external++;
            else if (f.getName().startsWith("FUN_")) unnamed++;
            else named++;
        }
        println("Named functions: " + named);
        println("Unnamed functions (FUN_): " + unnamed);
        println("Thunk functions: " + thunks);
        println("External functions: " + external);

        // Count C++ classes from RTTI
        HashSet<String> classNames = new HashSet<>();
        SymbolIterator si = st.getSymbolIterator();
        while (si.hasNext()) {
            Symbol s = si.next();
            Namespace ns = s.getParentNamespace();
            if (ns != null && !ns.isGlobal() && ns.getSymbol().getSymbolType() == SymbolType.CLASS) {
                classNames.add(ns.getName());
            }
        }
        println("C++ classes identified: " + classNames.size());

        // Count Function ID matches (via bookmarks or symbol properties)
        int fidMatches = 0;
        Iterator<Bookmark> bIter = bm.getBookmarksIterator("Function ID");
        while (bIter.hasNext()) {
            bIter.next();
            fidMatches++;
        }
        println("Function ID matches: " + fidMatches);

        ArrayList<String> sorted = new ArrayList<>(classNames);
        Collections.sort(sorted);
        int count = 0;
        for (String name : sorted) {
            if (count >= 50) {
                println("  ... and " + (classNames.size() - 50) + " more");
                break;
            }
            println("  " + name);
            count++;
        }

        println("=== End Stats ===");
    }
}
