import com.google.gson.*;
import com.google.gson.annotations.SerializedName;

// Run with Gson 2.10.1 (Minecraft 1.20.1):
// java -cp <gson.jar> scripts/GsonProbe.java
class GsonProbe {
    static class Fields {
        @SerializedName("show_crosshair") boolean showCrosshair;
        float base;
        int count;
        String name;
    }
    public static void main(String[] args) {
        Gson gson = new Gson();
        System.out.println("Gson JAR " + Gson.class.getProtectionDomain().getCodeSource().getLocation());
        for (String s : new String[]{"[\"laser\",\n  ]", "{\"base\": +0.15}",
                "{\"show_crosshair\": \"true\"}", "{unquoted:'value'}", "{\"x\":1,}",
                "[,1,,]", "{x=+1;y:TRUE}", "{\"count\":\"12\",\"name\":42}",
                "{\"show_crosshair\":\"FALSE\"}", "{\"show_crosshair\":\"yes\"}",
                "{\"show_crosshair\":1}", "{\"count\":\"2.5\"}", "{\"count\":\"2.0\"}",
                "{\"count\":2.5}", "{\"count\":\" 2 \"}", "{\"base\":\"no\"}"}) {
            try {
                JsonElement tree = gson.fromJson(s, JsonElement.class);
                System.out.print(s.replace("\n", "\\n") + " => tree=" + tree);
                if (tree.isJsonObject()) {
                    try { System.out.print(" fields=" + gson.toJson(gson.fromJson(s, Fields.class))); }
                    catch (RuntimeException e) { System.out.print(" fields=REJECT " + e.getClass().getSimpleName()); }
                    try { System.out.print(" treeFields=" + gson.toJson(gson.fromJson(tree, Fields.class))); }
                    catch (RuntimeException e) { System.out.print(" treeFields=REJECT " + e.getClass().getSimpleName()); }
                }
                System.out.println();
            } catch (RuntimeException e) { System.out.println(s + " => REJECT " + e.getClass().getSimpleName()); }
        }
    }
}
