package io.github.tmcleroy.musclemarch;

import org.libsdl.app.SDLActivity;

// SDL's activity loads the libraries below and calls the port's main() (engine/src/app/main.cpp)
// on its own thread; everything else is native.
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }
}
