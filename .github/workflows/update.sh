#!/bin/bash

directory=${APPIMAGE%$ARGV0}

if [ -w $directory ] ; then
	zenity --question --timeout=10 --title="yuzu updater" --text="New update available. Update now?" --icon-name=yuzu --window-icon=yuzu.svg --height=80 --width=400
	answer=$?

	if [ "$answer" -eq 0 ]; then 
		
			$APPDIR/usr/bin/AppImageUpdate $APPIMAGE && "$directory" "$@"
	
	elif [ "$answer" -eq 1 ]; then
		$APPDIR/AppRun-patched "$@"
	elif [ "$answer" -eq 5 ]; then
		$APPDIR/AppRun-patched "$@"
	fi

else
	zenity --error --timeout=5 --text="Cannot update in $directory" --title="Update Error" --width=500 --width=200
	$APPDIR/AppRun-patched "$@"
fi
exit 0
