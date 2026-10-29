// SPDX-License-Identifier: GPL-3.0-or-later
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class LanternDecompile extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File out = new File(args[0]);
		out.mkdirs();
		if (args.length == 1) {
			try (PrintWriter w = new PrintWriter(new File(out, "functions.csv"))) {
				w.println("address,size,name,callers");
				for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
					w.printf("%x,%x,%s,%d%n", f.getEntryPoint().getOffset(), f.getBody().getNumAddresses(),
						f.getName(), f.getSymbol().getReferenceCount());
				}
			}
			println("Lantern export: " + currentProgram.getFunctionManager().getFunctionCount() + " functions");
			return;
		}
		DecompInterface d = new DecompInterface();
		d.openProgram(currentProgram);
		for (int i = 1; i < args.length; i++) {
			Function f = getFunctionContaining(toAddr(Long.parseUnsignedLong(args[i].replace("0x", ""), 16)));
			if (f == null) {
				println("no function at " + args[i]);
				continue;
			}
			DecompileResults r = d.decompileFunction(f, 120, monitor);
			String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "/* " + r.getErrorMessage() + " */";
			File file = new File(out, String.format("%x_%s.c", f.getEntryPoint().getOffset(), f.getName()));
			try (PrintWriter w = new PrintWriter(file)) {
				w.print(c);
			}
			println("wrote " + file);
		}
		d.dispose();
	}
}
