// SPDX-License-Identifier: GPL-3.0-or-later
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.parallel.DecompilerCallback;
import ghidra.app.decompiler.parallel.ParallelDecompiler;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.util.task.TaskMonitor;

public class LanternDump extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File out = new File(args[0]);
		out.mkdirs();
		long lo = args.length > 1 ? Long.parseUnsignedLong(args[1].replace("0x", ""), 16) : 0;
		long hi = args.length > 2 ? Long.parseUnsignedLong(args[2].replace("0x", ""), 16) : Long.MAX_VALUE;
		long shard = 0x100000;
		TreeMap<Long, List<Function>> shards = new TreeMap<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			long a = f.getEntryPoint().getOffset();
			if (a >= lo && a < hi && !f.isThunk()) {
				shards.computeIfAbsent(a / shard, k -> new ArrayList<>()).add(f);
			}
		}
		DecompileOptions options = new DecompileOptions();
		options.grabFromProgram(currentProgram);
		int done = 0, failed = 0;
		for (Map.Entry<Long, List<Function>> e : shards.entrySet()) {
			File file = new File(out, String.format("%07x.c", e.getKey() * shard));
			if (file.exists()) {
				done += e.getValue().size();
				continue;
			}
			DecompilerCallback<String[]> callback = new DecompilerCallback<>(currentProgram, d -> {
				d.setOptions(options);
				d.toggleSyntaxTree(false);
				d.setSimplificationStyle("decompile");
			}) {
				@Override
				public String[] process(DecompileResults r, TaskMonitor m) {
					Function f = r.getFunction();
					String head = String.format("// %x %s", f.getEntryPoint().getOffset(), f.getName());
					if (r.decompileCompleted()) {
						return new String[] { head, r.getDecompiledFunction().getC() };
					}
					return new String[] { head, "// failed: " + r.getErrorMessage() + "\n" };
				}
			};
			callback.setTimeout(90);
			List<String[]> results = ParallelDecompiler.decompileFunctions(callback, e.getValue(), monitor);
			callback.dispose();
			results.sort((x, y) -> Long.compareUnsigned(Long.parseUnsignedLong(x[0].split(" ")[1], 16),
				Long.parseUnsignedLong(y[0].split(" ")[1], 16)));
			File tmp = new File(out, file.getName() + ".tmp");
			try (PrintWriter w = new PrintWriter(tmp, "UTF-8")) {
				for (String[] r : results) {
					w.println(r[0]);
					w.println(r[1]);
					if (r[1].startsWith("// failed")) {
						failed++;
					}
				}
			}
			tmp.renameTo(file);
			done += e.getValue().size();
			println(String.format("Lantern dump: %s, %d functions so far, %d failed", file.getName(), done, failed));
		}
		println("Lantern dump done: " + done + " functions, " + failed + " failed");
	}
}
