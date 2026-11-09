// Runs the setup page's own time zone script (lifted from the rendered page).
// posixTz: known browser offsets. posixRule: for real zones, the rule the page works out must give the same
// UTC offset as the zone database for every half hour of this year and the next, as the C library reads it.
const fs = require('fs');
const { execFileSync } = require('child_process');
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
let fails = 0, checks = cases.length;
for (const [offset, want] of cases) {
  const got = posixTz(offset);
  if (got !== want) {
    fails++;
    console.log(`FAIL posixTz(${offset}) = ${got}, want ${want}`);
  }
}

const YEAR = 2026;
const ZONES = {
  'Europe/Berlin': '<+01>-1<+02>-2,M3.5.0/2,M10.5.0/3',
  'Europe/London': '<+00>0<+01>-1,M3.5.0/1,M10.5.0/2',
  'America/New_York': '<-05>5<-04>4,M3.2.0/2,M11.1.0/2',
  'America/St_Johns': '<-0330>3:30<-0230>2:30,M3.2.0/2,M11.1.0/2',
  'Australia/Sydney': '<+10>-10<+11>-11,M10.1.0/2,M4.1.0/3',
  'Australia/Lord_Howe': '<+1030>-10:30<+11>-11,M10.1.0/2,M4.1.0/2',
  'Pacific/Auckland': '<+12>-12<+13>-13,M9.5.0/2,M4.1.0/3',
  'Asia/Tbilisi': '<+04>-4',
  'Asia/Kolkata': '<+0530>-5:30',
  'UTC': 'UTC0',
};
const VERIFY = `
import os, sys, time
from datetime import datetime, timezone
from zoneinfo import ZoneInfo
zone, rule, year = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.environ['TZ'] = rule
time.tzset()
z = ZoneInfo(zone)
start = int(datetime(year, 1, 1, tzinfo=timezone.utc).timestamp())
end = int(datetime(year + 2, 1, 1, tzinfo=timezone.utc).timestamp())
bad = [t for t in range(start, end, 1800)
       if time.localtime(t).tm_gmtoff != datetime.fromtimestamp(t, z).utcoffset().total_seconds()]
print(len(bad), datetime.fromtimestamp(bad[0], timezone.utc).isoformat() if bad else '')
`;
for (const [zone, want] of Object.entries(ZONES)) {
  checks += 2;
  const got = execFileSync(process.execPath, ['-e', `${lib[1]}\nprocess.stdout.write(posixRule(${YEAR}));`], { env: { ...process.env, TZ: zone }, encoding: 'utf8' });
  if (got !== want) {
    fails++;
    console.log(`FAIL posixRule in ${zone} = ${got}, want ${want}`);
  }
  const [wrong, first] = execFileSync('python3', ['-I', '-c', VERIFY, zone, got, String(YEAR)], { encoding: 'utf8' }).trim().split(' ');
  if (wrong !== '0') {
    fails++;
    console.log(`FAIL ${zone}: "${got}" disagrees with the zone database for ${wrong} half hours, first at ${first}`);
  }
}
console.log(`${checks} tz checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
