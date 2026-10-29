// SPDX-License-Identifier: GPL-3.0-or-later
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class LanternSeed extends GhidraScript {
	@Override
	protected void run() throws Exception {
		File dir = new File(getScriptArgs()[0]);
		AddressSet starts = new AddressSet();
		try (BufferedReader r = new BufferedReader(new FileReader(new File(dir, "functions.csv")))) {
			for (String line; (line = r.readLine()) != null;) {
				starts.add(toAddr(Long.parseUnsignedLong(line.split(",")[0], 16)));
			}
		}
		new DisassembleCommand(starts, null, true).applyTo(currentProgram, monitor);
		int made = 0;
		for (Address a : starts.getAddresses(true)) {
			monitor.checkCancelled();
			if (getFunctionAt(a) == null && createFunction(a, null) != null) {
				made++;
			}
		}
		int labels = 0;
		try (BufferedReader r = new BufferedReader(new FileReader(new File(dir, "labels.csv")))) {
			for (String line; (line = r.readLine()) != null;) {
				String[] f = line.split(",", -1);
				Address a = toAddr(Long.parseUnsignedLong(f[0], 16));
				String name = f[2];
				if (f[1].equals("slot")) {
					clearListing(a, a.add(7));
					createData(a, PointerDataType.dataType);
					createLabel(a, "__imp_" + name, true, SourceType.IMPORTED);
				}
				else {
					Function fn = getFunctionAt(a);
					if (fn == null) {
						disassemble(a);
						fn = createFunction(a, null);
					}
					if (fn != null) {
						fn.setName(name, SourceType.IMPORTED);
					}
					else {
						createLabel(a, name, true, SourceType.IMPORTED);
					}
				}
				labels++;
			}
		}
		for (String heavy : new String[] { "Stack", "Function ID", "Library Identification",
			"Decompiler Parameter ID", "Aggressive Instruction Finder", "Embedded Media", "Variadic Function Signature Override" }) {
			if (getCurrentAnalysisOptionsAndValues(currentProgram).containsKey(heavy)) {
				setAnalysisOption(currentProgram, heavy, "false");
			}
		}
		println("Lantern seed: " + made + " functions, " + labels + " labels");
	}
}
