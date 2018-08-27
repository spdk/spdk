# Visual Studio Code IDE with SPDK {#vscode_tut}

## Prerequisites


### Visual Studio Code

Download Visual studio Code from: https://code.visualstudio.com/

### Git

Downlaod Git for windows: https://git-scm.com/download/win

### Share your spdk folder from remote machine

You can do that either by samba share, rsync or sftp.
You should share a directory which contains at least two subdirectories:
```
share/spdk    # Directory with spdk project
share/include # This folder should link to /usr/include on remote machine.

#Optional:
share/rocksdb # Rocksdb directory
# etc...
```

## Opening SPDK workspace
Required plugins:
[C/C++ IntelliSense, debugging, and code browsing.](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools)



In `spdk/.vscode/spdk.code-workspace` you'll find workspace file for spdk project.

The file contains configuration for InteliSense for c/cpp like this:

```
{
	"folders": [
		// This path points to spdk workspace
		{
			"path": "../../spdk"
		},
		// Uncomment for additional folders in workspace:
		//{
		//	"path": "../../rocksdb"
		//},
		//{
		//	"path": "../../spdk/dpdk"
		//}
	],
	"settings": {
		// This file will contain symbols database, put it on your fastest drive.
		"C_Cpp.default.browse.databaseFilename": "${env.USERPROFILE}\\.vscode\\symbols.db",
		"C_Cpp.default.browse.limitSymbolsToIncludedHeaders": false,
		"C_Cpp.default.browse.path": [ "${workspaceFolder}", "${workspaceFolder}/../../include" ],
		"C_Cpp.default.intelliSenseMode": "clang-x64",
		"C_Cpp.intelliSenseEngine": "Tag Parser",
		"C_Cpp.default.cppStandard": "c++17",
		"C_Cpp.default.cStandard": "c11",
		"C_Cpp.default.defines": [
			"_DEBUG",
			"UNICODE",
			"_UNICODE",
			"__x86_64__",
			"__GNUC__"
		]
	},
}
```

## Internal terminal

For terminal support you need to intall ssh.
I'm using the one that comes with Git, but you can probably try others.


Copy following setttings to user settings:

```
	// Point to your ssh executable
	"terminal.integrated.shell.windows": "C:\\Program Files\\Git\\usr\\bin\\ssh.exe",
	//
	"terminal.integrated.shellArgs.windows": [
		// Replace 192.168.0.1 with you remote server address:
		"192.168.0.1",
		// Replace share/spdk with path to spdk project on remote machine:
		"-t", "cd share/spdk; exec $SHELL"
	],
```

## Remote debugger
Required plugins:
[Native Debug](https://marketplace.visualstudio.com/items?itemName=webfreak.debug)

GDB configurations:

```
{
    "version": "0.2.0",
    "configurations": [
        {
            "type": "gdb",
            "request": "launch",
            "name": "blob_ut",
            "target": "./test/unit/lib/blob/blob.c/blob_ut",
            "cwd": "${workspaceRoot}",
            "ssh": {
                "host": "10.102.17.111",
                "cwd": "/home/<user>/git/spdk",
                "keyfile": "C:/Users/<user>/.ssh/id_rsa",
                "user": "<user>"
            },
        },
		// ...
    ]
}
```

## Remote tasks

In .vscode/tasks you'll find definition of tasks:

Remember to replace ip with your remote server address

```
{
    "version": "2.0.0",
    "windows": {
        "options": {
            "shell": {
                "executable": "C:\\Program Files\\Git\\usr\\bin\\ssh.exe",
                "args": [ "192.168.0.1" ] // Replace with your server address
            },
        }
    },
    "tasks": [
        {
            "label": "make",
            "type": "shell",
            "command": "cd git/spdk && make",
            "group": {
                "kind": "build",
                "isDefault": true
            },
            "presentation": {
                "reveal": "always",
                "panel": "new"
            }
        },
        {
            "label": "make clean",
            "type": "shell",
            "command": "cd git/spdk && make clean"
        }
    ]
}

```
