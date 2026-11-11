// PS5 A2UI's entry in websrv's Homebrew Launcher (the "Homebrew Launcher" tile, or
// http://<console>:8080 in a browser). websrv finds it in /data/homebrew/PS5AgenticUI/ with
// sce_sys/icon0.png, and launches eboot.elf from this folder: the app keeps its key
// (credentials.json) and the Mesa libraries here.
async function main() {
  const dir = window.workingDir;
  return {
    mainText: "PS5 A2UI",
    secondaryText: "An assistant that answers with console screens",
    onclick: async () => ({
      path: dir + "/eboot.elf",
      cwd: dir,
      env: { LD_LIBRARY_PATH: dir },
    }),
  };
}
