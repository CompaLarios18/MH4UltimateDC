// Export Ghidra pseudo-C for every function, on several threads, for MH4UltimateDC agents.
//
// The same decomp/<addr>.c files as ExportForAgents' "all" mode, but each thread has its own
// decompiler, and functions whose file already exists are skipped (so an interrupted export
// resumes). Run it on a program that is already analysed and saved:
//   analyzeHeadless <proj_dir> <proj_name> -process <program> -noanalysis -readOnly \
//     -scriptPath <dir> -postScript ExportPseudoCParallel.java <out_dir> [threads]
// Keep <out_dir> OUTSIDE any git repo: it is derived from game code.
//
//@category MH4UltimateDC
//@author MH4UltimateDC contributors

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.util.task.TaskMonitor;

public class ExportPseudoCParallel extends GhidraScript {
    private static final int DECOMPILE_TIMEOUT_SECONDS = 60;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: ExportPseudoCParallel.java <out_dir> [threads]");
        }
        File dir = new File(args[0], "decomp");
        dir.mkdirs();
        int threads = args.length > 1 ? Integer.parseInt(args[1]) : Math.max(1, Runtime.getRuntime().availableProcessors() - 2);

        List<Function> targets = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            File out = new File(dir, String.format("%08x.c", f.getEntryPoint().getOffset()));
            if (!f.isThunk() && !f.isExternal() && !out.exists()) {
                targets.add(f);
            }
        }
        println(String.format("%d functions to decompile on %d threads", targets.size(), threads));

        AtomicInteger next = new AtomicInteger(), done = new AtomicInteger(), ok = new AtomicInteger();
        ExecutorService pool = Executors.newFixedThreadPool(threads);
        for (int t = 0; t < threads; t++) {
            pool.submit(() -> {
                DecompInterface di = new DecompInterface();
                di.toggleCCode(true);
                di.toggleSyntaxTree(false);
                di.setSimplificationStyle("decompile");
                if (!di.openProgram(currentProgram)) {
                    printerr("decompiler failed to start: " + di.getLastMessage());
                    return;
                }
                try {
                    for (int i = next.getAndIncrement(); i < targets.size() && !monitor.isCancelled();
                         i = next.getAndIncrement()) {
                        Function f = targets.get(i);
                        DecompileResults r = di.decompileFunction(f, DECOMPILE_TIMEOUT_SECONDS, TaskMonitor.DUMMY);
                        String body;
                        if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null) {
                            body = r.getDecompiledFunction().getC();
                            ok.incrementAndGet();
                        } else {
                            body = "/* decompile failed: " + (r == null ? "no result" : r.getErrorMessage()) + " */\n";
                        }
                        String name = String.format("%08x.c", f.getEntryPoint().getOffset());
                        Files.write(new File(dir, name).toPath(), body.getBytes(StandardCharsets.UTF_8));
                        int n = done.incrementAndGet();
                        if (n % 1000 == 0) {
                            println(String.format("%d / %d", n, targets.size()));
                        }
                    }
                } catch (Exception e) {
                    printerr("decompile thread: " + e);
                } finally {
                    di.dispose();
                }
            });
        }
        pool.shutdown();
        while (!pool.awaitTermination(10, TimeUnit.SECONDS)) {
            if (monitor.isCancelled()) {
                pool.shutdownNow();
            }
        }
        println(String.format("decompiled %d of %d", ok.get(), targets.size()));
    }
}
