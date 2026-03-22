@echo off

:: ÉèÖÃ»ù´¡Â·¾¶
set "TARGET_BASE=C:\ResourceLibrary\Code\C++\Libraries\x64-windows"
set "SOURCE_BASE=."

xcopy /E /I /Y "%SOURCE_BASE%/include/zen_eventbus" "%TARGET_BASE%\include\zen_eventbus" 
xcopy /Y /F "%SOURCE_BASE%/bin\x64\Debug/zen_eventbusd.dll" "%TARGET_BASE%\debug\bin\"
xcopy /Y /F "%SOURCE_BASE%/bin\x64\Debug/zen_eventbusd.pdb" "%TARGET_BASE%\debug\bin\"
xcopy /Y /F "%SOURCE_BASE%/bin\x64\Release/zen_eventbus.dll" "%TARGET_BASE%\bin\"
xcopy /Y /F "%SOURCE_BASE%/bin\x64\Release/zen_eventbus.pdb" "%TARGET_BASE%\bin\"

echo File copied successfully!
pause
