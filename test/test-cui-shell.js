const { launchCuiShell } = require('../index.js');

const shells=['cmd','powershell']
const currentDirectory = __dirname;
for (let shell of shells){

    const status = launchCuiShell(shell, currentDirectory);

    if (status) {
        console.log(`✅ ${shell}测试成功！`);
    }else{
        console.log(`❌ ${shell}测试失败！`);
    }

}
