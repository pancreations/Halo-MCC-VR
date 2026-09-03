// Move the currently processed program into a named project folder.
// @category HaloMCCVR.RE

import ghidra.app.script.GhidraScript;
import ghidra.framework.model.DomainFile;
import ghidra.framework.model.DomainFolder;

public class MoveCurrentProgramToFolder extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length != 1 || args[0].isBlank()) {
            throw new IllegalArgumentException(
                "usage: MoveCurrentProgramToFolder.java <folder-name>");
        }

        DomainFolder root = state.getProject()
            .getProjectData().getRootFolder();
        DomainFolder destination = root.getFolder(args[0]);
        if (destination == null) {
            destination = root.createFolder(args[0]);
        }

        DomainFile file = currentProgram.getDomainFile();
        if (!destination.equals(file.getParent())) {
            // A headless -process keeps the source DomainFile open, so it
            // cannot be moved during the script. Copying preserves the fully
            // analyzed database and lets a later BSim pass target only the
            // requested project folder.
            file.copyTo(destination, monitor);
        }
        println("Copied " + file.getName() + " to /" + args[0]);
    }
}
