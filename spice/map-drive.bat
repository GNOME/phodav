net use * http://localhost:9843/

REG ADD "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\MountPoints2\##localhost@9843#DavWWWRoot" /v  "_LabelFromReg" /t REG_SZ /d "Spice client" /f