const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const { execFileSync } = require("child_process");

function quoteWindowsArgument(value) {
  if (!/[\s"]/.test(value)) return value;
  return `"${value.replace(/(\\*)"/g, '$1$1\\"').replace(/(\\+)$/, "$1$1")}"`;
}

function waitForFile(filePath, timeoutMs) {
  const startedAt = Date.now();
  return new Promise((resolve, reject) => {
    const check = () => {
      if (fs.existsSync(filePath)) {
        resolve();
        return;
      }
      if (Date.now() - startedAt >= timeoutMs) {
        reject(new Error(`Timed out waiting for child result: ${filePath}`));
        return;
      }
      setTimeout(check, 50);
    };
    check();
  });
}

if (process.argv[2] === "--child") {
  fs.writeFileSync(
    process.argv[3],
    JSON.stringify({ pid: process.pid, parentPid: process.ppid }),
    "utf8",
  );
  process.exit(0);
}

async function main() {
  if (process.platform !== "win32") {
    console.log("Skipped: Explorer launch is only supported on Windows.");
    return;
  }

  const addon = require("../build/Release/ztools_native.node");
  assert.throws(
    () => addon.launchViaExplorer({}),
    /target must be a non-empty string/,
  );
  assert.throws(
    () =>
      addon.launchViaExplorer({ target: process.execPath, showCommand: 12 }),
    /showCommand must be an integer between 0 and 11/,
  );

  const resultPath = path.join(
    os.tmpdir(),
    `ztools-explorer-launch-${process.pid}-${Date.now()}.json`,
  );
  const parameters = [__filename, "--child", resultPath]
    .map(quoteWindowsArgument)
    .join(" ");

  try {
    const launchResult = await addon.launchViaExplorer({
      target: process.execPath,
      parameters,
      workingDirectory: __dirname,
      showCommand: 0,
    });
    assert.strictEqual(
      launchResult.success,
      true,
      `Explorer dispatch failed at ${launchResult.stage} (HRESULT ${launchResult.hresult})`,
    );

    await waitForFile(resultPath, 10000);
    const childResult = JSON.parse(fs.readFileSync(resultPath, "utf8"));
    const parentName = execFileSync(
      "powershell.exe",
      [
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        `(Get-Process -Id ${childResult.parentPid} -ErrorAction Stop).ProcessName`,
      ],
      { encoding: "utf8" },
    ).trim();

    assert.strictEqual(
      parentName.toLowerCase(),
      "explorer",
      `Expected explorer.exe as parent, got ${parentName} (PID ${childResult.parentPid})`,
    );
    assert.notStrictEqual(childResult.parentPid, process.pid);
    console.log("Explorer launch result:", launchResult);
    console.log("Child process:", childResult);
    console.log("Parent process:", parentName);
  } finally {
    fs.rmSync(resultPath, { force: true });
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
