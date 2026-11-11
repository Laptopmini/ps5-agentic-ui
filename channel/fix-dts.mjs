// Post-process the emitted declarations: rewrite relative .ts import/export specifiers to .js, which
// TypeScript resolves to the sibling .d.ts. tsc keeps the repo's .ts extensions in the .d.ts (and
// rewriteRelativeImportExtensions doesn't apply under emitDeclarationOnly), which would break type
// resolution for a consumer that imports this package.
import { readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";

function walk(dir) {
  for (const entry of readdirSync(dir)) {
    const p = `${dir}/${entry}`;
    if (statSync(p).isDirectory()) walk(p);
    else if (p.endsWith(".d.ts")) {
      const src = readFileSync(p, "utf8");
      const out = src.replace(/\.ts"/g, '.js"');
      if (out !== src) writeFileSync(p, out);
    }
  }
}

walk(new URL("./dist", import.meta.url).pathname);
