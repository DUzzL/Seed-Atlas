// Launcher for an unobfuscated (26.1+) vanilla dedicated server that, once the
// server is ready, prints vanilla's own concentric-ring (stronghold) position
// list for the overworld in placement order, then leaves the server running
// as a normal console server.
//
// Output lines: "RINGDUMP <set id> <index> <chunkX> <chunkZ>",
// "RINGDUMP DEFAULT <end_portal_frame default state>" and "RINGDUMP END".
//
// Needs: --add-opens java.base/java.lang=ALL-UNNAMED (to read the server
// thread's Runnable, which holds the only reference to the server instance).

import java.lang.reflect.Field;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;

import net.minecraft.core.Holder;
import net.minecraft.server.MinecraftServer;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.chunk.ChunkGeneratorStructureState;
import net.minecraft.world.level.levelgen.structure.StructureSet;
import net.minecraft.world.level.levelgen.structure.placement.ConcentricRingsStructurePlacement;
import net.minecraft.world.level.levelgen.structure.placement.StructurePlacement;

public class RingDump {
    public static void main(String[] args) throws Exception {
        Thread watcher = new Thread(RingDump::watch, "RingDump");
        watcher.setDaemon(true);
        watcher.start();
        net.minecraft.server.Main.main(args);
    }

    private static Thread findServerThread() {
        for (Thread t : Thread.getAllStackTraces().keySet()) {
            if ("Server thread".equals(t.getName())) return t;
        }
        return null;
    }

    private static MinecraftServer serverOf(Thread t) throws Exception {
        Field holderF = Thread.class.getDeclaredField("holder");
        holderF.setAccessible(true);
        Object holder = holderF.get(t);
        Field taskF = holder.getClass().getDeclaredField("task");
        taskF.setAccessible(true);
        Object task = taskF.get(holder);
        for (Field f : task.getClass().getDeclaredFields()) {
            f.setAccessible(true);
            Object v = f.get(task);
            if (v instanceof AtomicReference<?> ref) {
                Object s = ref.get();
                if (s instanceof MinecraftServer ms) return ms;
            }
        }
        return null;
    }

    private static void watch() {
        try {
            MinecraftServer server = null;
            while (server == null) {
                Thread t = findServerThread();
                if (t != null) server = serverOf(t);
                if (server == null) Thread.sleep(200);
            }
            Field ready = MinecraftServer.class.getDeclaredField("isReady");
            ready.setAccessible(true);
            while (!ready.getBoolean(server)) Thread.sleep(200);

            ChunkGeneratorStructureState state =
                server.overworld().getChunkSource().getGeneratorState();
            StringBuilder out = new StringBuilder();
            for (Holder<StructureSet> set : state.possibleStructureSets()) {
                StructurePlacement p = set.value().placement();
                if (!(p instanceof ConcentricRingsStructurePlacement rings)) continue;
                String id = set.unwrapKey().map(k -> k.identifier().toString()).orElse("?");
                List<ChunkPos> list = state.getRingPositionsFor(rings);
                if (list == null) continue;
                for (int i = 0; i < list.size(); i++) {
                    ChunkPos c = list.get(i);
                    out.append("RINGDUMP ").append(id).append(' ').append(i).append(' ')
                       .append(c.x()).append(' ').append(c.z()).append('\n');
                }
            }
            // 26.1+ chunk palettes store default-state blocks as a bare id; report the
            // default state so the reader can resolve such entries from vanilla itself.
            out.append("RINGDUMP DEFAULT ")
               .append(net.minecraft.world.level.block.Blocks.END_PORTAL_FRAME.defaultBlockState())
               .append('\n');
            out.append("RINGDUMP END\n");
            System.out.print(out);
            System.out.flush();
        } catch (Throwable e) {
            System.out.println("RINGDUMP ERROR " + e);
            e.printStackTrace(System.out);
            System.out.flush();
        }
    }
}
