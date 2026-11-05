package io.github.mogeldev.scda;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

import org.libsdl.app.SDLActivity;

// A mission starts as a quick reboot of the title: the game process starts this activity in its own process and
// exits, this one makes sure the old process is gone and starts the game again (src/android/host.cpp, Restart).
public class Relaunch extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        int pid = getIntent().getIntExtra("pid", 0);
        if (pid > 0) Process.killProcess(pid);
        startActivity(new Intent(this, SDLActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        finish();
        Runtime.getRuntime().exit(0);
    }
}
