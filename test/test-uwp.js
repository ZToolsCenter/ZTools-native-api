const { UwpManager } = require('../index.js');

console.log('Testing getUwpApps...');
const apps = UwpManager.getUwpApps();
console.log('Total UWP apps:', apps.length);

// Filter to show only apps with properly resolved names (not familyName fallback)
const userApps = apps.filter(app => {
  // Skip apps whose name looks like a familyName (contains _ followed by hash)
  return !app.name.match(/^[a-f0-9\-]{36}_/i) &&
         !app.name.match(/_[a-z0-9]{13}$/i);
});

console.log('\nUser-visible apps (' + userApps.length + '):');
userApps.forEach((app, i) => {
  console.log(`  [${i + 1}] ${app.name}`);
  console.log(`      AppId: ${app.appId}`);
  console.log(`      Icon: ${app.icon}`);
});

// Test launching calculator
console.log('\n--- Testing launchUwpApp ---');
const calculator = apps.find(app => app.appId.includes('WindowsCalculator'));
if (calculator) {
  console.log('Found calculator:', calculator.name, calculator.appId);
  console.log('Launching...');
  const result = UwpManager.launchUwpApp(calculator.appId);
  console.log('Launch result:', result);
  if (!result.success) {
    process.exitCode = 1;
  }
} else {
  console.log('Calculator not found in app list');
}
