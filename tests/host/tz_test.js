// Runs the page's own posixTz() (lifted from the rendered page) on known browser offsets.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
const lib = html.match(/<script id="tzlib">([\s\S]*?)<\/script>/);
if (!lib) {
  console.log('FAIL the page has no <script id="tzlib">');
  process.exit(1);
}
const posixTz = new Function(lib[1] + '\nreturn posixTz;')();
const cases = [
  [0, 'UTC0'], [-240, '<+04>-4'], [330, '<-0530>5:30'], [-345, '<+0545>-5:45'],
  [300, '<-05>5'], [-60, '<+01>-1'], [-330, '<+0530>-5:30'], [-840, '<+14>-14'], [600, '<-10>10'],
];
let fails = 0;
for (const [offset, want] of cases) {
  const got = posixTz(offset);
  if (got !== want) {
    fails++;
    console.log(`FAIL posixTz(${offset}) = ${got}, want ${want}`);
  }
}
console.log(`${cases.length} tz checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
