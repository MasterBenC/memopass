const fs = require("fs");
const path = require("path");

async function main() {
  const convert = require("lv_font_conv/lib/convert");
  const font = process.argv[2];
  const symbolsPath = process.argv[3];
  const size = Number(process.argv[4]);
  const bpp = Number(process.argv[5]);
  const name = process.argv[6];
  const output = process.argv[7];
  const symbols = fs.readFileSync(symbolsPath, "utf8");
  await convert({
    font: [{ source_path: font, ranges: [{ symbols }] }],
    size,
    bpp,
    format: "lvgl",
    output,
    lv_font_name: name,
    lv_include: "lvgl.h",
    no_compress: true,
    no_kerning: true,
    bpp_class: bpp,
  });
  const stat = fs.statSync(output);
  console.log(path.basename(output), stat.size);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
