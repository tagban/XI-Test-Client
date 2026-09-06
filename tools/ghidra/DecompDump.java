// Decompiles the functions whose hex addresses are listed in the file named by
// the MOGHOUSE_TARGETS env var (one per line, e.g. 10289bb0), writing the C to
// the file named by MOGHOUSE_OUT. Run headless with -postScript DecompDump.java.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.*;
import java.nio.file.*;

public class DecompDump extends GhidraScript {
    public void run() throws Exception {
        String targets = System.getenv("MOGHOUSE_TARGETS");
        String out = System.getenv("MOGHOUSE_OUT");
        if (targets == null || out == null) { println("set MOGHOUSE_TARGETS and MOGHOUSE_OUT"); return; }

        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        StringBuilder sb = new StringBuilder();

        for (String line : Files.readAllLines(Paths.get(targets))) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            Address a = currentProgram.getAddressFactory().getAddress(line);
            Function f = getFunctionAt(a);
            if (f == null) f = getFunctionContaining(a);
            if (f == null) {
                // try to create one
                disassemble(a);
                createFunction(a, null);
                f = getFunctionAt(a);
            }
            sb.append("//==== ").append(line).append("  ")
              .append(f != null ? f.getName() : "(no function)").append(" ====\n");
            if (f == null) { sb.append("// could not resolve a function here\n\n"); continue; }
            DecompileResults r = di.decompileFunction(f, 90, monitor);
            if (r != null && r.decompileCompleted()) {
                sb.append(r.getDecompiledFunction().getC()).append("\n");
            } else {
                sb.append("// decompile failed: ").append(r == null ? "null" : r.getErrorMessage()).append("\n\n");
            }
        }
        Files.write(Paths.get(out), sb.toString().getBytes());
        println("wrote " + out);
    }
}
