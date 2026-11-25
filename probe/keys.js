// wcinput: raw mode の Node が受けた字を記録する (ink と同じ読み方)。Ctrl+C 2 回か 90 秒で終わる
// 使い方: node keys.js <記録>
const fs = require('fs');
const log = process.argv[2];
fs.writeFileSync(log, 'start isTTY=' + process.stdin.isTTY + '\n');
process.stdin.setRawMode(true);
process.stdin.setEncoding('utf8');
process.stdin.resume();
let n = 0;
process.stdin.on('data', d => {
  fs.appendFileSync(log, 'data ' + JSON.stringify(d) + '\n');
  if (d.includes('\x03') && ++n >= 2) {
    fs.appendFileSync(log, 'exit\n');
    process.exit(0);
  }
});
setTimeout(() => { fs.appendFileSync(log, 'timeout\n'); process.exit(1); }, 90000);
