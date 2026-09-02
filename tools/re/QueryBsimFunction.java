// Query one analyzed function against a BSim database from analyzeHeadless.
// @category HaloMCCVR.RE

import java.net.URL;
import java.util.Iterator;

import ghidra.app.script.GhidraScript;
import ghidra.features.bsim.query.BSimClientFactory;
import ghidra.features.bsim.query.FunctionDatabase;
import ghidra.features.bsim.query.GenSignatures;
import ghidra.features.bsim.query.description.DescriptionManager;
import ghidra.features.bsim.query.description.ExecutableRecord;
import ghidra.features.bsim.query.description.FunctionDescription;
import ghidra.features.bsim.query.protocol.QueryNearest;
import ghidra.features.bsim.query.protocol.ResponseNearest;
import ghidra.features.bsim.query.protocol.SimilarityNote;
import ghidra.features.bsim.query.protocol.SimilarityResult;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class QueryBsimFunction extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length != 2) {
            throw new IllegalArgumentException(
                "usage: QueryBsimFunction.java <address> <bsim-url>");
        }

        Address address = currentProgram.getAddressFactory().getAddress(args[0]);
        Function function = address == null ? null : getFunctionAt(address);
        if (function == null) {
            throw new IllegalArgumentException(
                "no function starts at " + args[0]);
        }

        URL url = BSimClientFactory.deriveBSimURL(args[1]);
        try (FunctionDatabase database =
                BSimClientFactory.buildClient(url, false)) {
            if (!database.initialize()) {
                throw new IllegalStateException(database.getLastError().message);
            }

            GenSignatures generator = new GenSignatures(false);
            try {
                generator.setVectorFactory(database.getLSHVectorFactory());
                generator.openProgram(
                    currentProgram, null, null, null, null, null);
                generator.scanFunction(function);

                DescriptionManager manager = generator.getDescriptionManager();
                QueryNearest query = new QueryNearest();
                query.manage = manager;
                query.max = 25;
                // Cross-compiler and cross-architecture H2EK-to-retail queries
                // can rank below the ordinary same-architecture threshold.
                // Emit the broad diagnostic set; callers must verify ABI,
                // structure, and a unique loaded-image signature separately.
                query.thresh = 0.0;
                query.signifthresh = 0.0;

                ResponseNearest response = query.execute(database);
                if (response == null) {
                    throw new IllegalStateException(
                        database.getLastError().message);
                }

                println("BSIM_QUERY source=" + currentProgram.getName() +
                    " address=" + function.getEntryPoint() +
                    " name=" + function.getName());
                Iterator<SimilarityResult> results = response.result.iterator();
                while (results.hasNext()) {
                    SimilarityResult result = results.next();
                    FunctionDescription base = result.getBase();
                    println("BSIM_BASE executable=" +
                        base.getExecutableRecord().getNameExec() +
                        " address=0x" + Long.toHexString(base.getAddress()) +
                        " name=" + base.getFunctionName());
                    Iterator<SimilarityNote> notes = result.iterator();
                    while (notes.hasNext()) {
                        SimilarityNote note = notes.next();
                        FunctionDescription match = note.getFunctionDescription();
                        ExecutableRecord executable = match.getExecutableRecord();
                        println("BSIM_MATCH executable=" + executable.getNameExec() +
                            " md5=" + executable.getMd5() +
                            " address=0x" + Long.toHexString(match.getAddress()) +
                            " name=" + match.getFunctionName() +
                            " similarity=" + note.getSimilarity() +
                            " significance=" + note.getSignificance());
                    }
                }
            }
            finally {
                generator.dispose();
            }
        }
    }
}
