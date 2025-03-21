/*
 *   Copyright (c) 2022 Project CHIP Authors
 *   All rights reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */

#include "InteractiveCommands.h"

#include <readline/history.h>
#include <readline/readline.h>

char kInteractiveModeName[]                            = "";
constexpr const char * kInteractiveModePrompt          = ">>> ";
constexpr uint8_t kInteractiveModeArgumentsMaxLength   = 32;
constexpr const char * kInteractiveModeHistoryFilePath = "/tmp/chip_tool_history";
constexpr const char * kInteractiveModeStopCommand     = "quit()";

namespace {

bool gIsCommandRunning = false;

void ClearLine()
{
    printf("\r\x1B[0J"); // Move cursor to the beginning of the line and clear from cursor to end of the screen
}

void ENFORCE_FORMAT(3, 0) LoggingCallback(const char * module, uint8_t category, const char * msg, va_list args)
{
    ClearLine();
    chip::Logging::Platform::LogV(module, category, msg, args);
    ClearLine();

    if (gIsCommandRunning == false)
    {
        rl_forced_update_display();
    }
}
} // namespace

char * GetCommand(char * command)
{
    if (command != nullptr)
    {
        free(command);
        command = nullptr;
    }

    command = readline(kInteractiveModePrompt);

    // Do not save empty lines
    if (command != nullptr && *command)
    {
        add_history(command);
        write_history(kInteractiveModeHistoryFilePath);
    }

    return command;
}

CHIP_ERROR InteractiveStartCommand::RunCommand()
{
    read_history(kInteractiveModeHistoryFilePath);

    // Logs needs to be redirected in order to refresh the screen appropriately when something
    // is dumped to stdout while the user is typing a command.
    chip::Logging::SetLogRedirectCallback(LoggingCallback);

    char * command = nullptr;
    while (true)
    {
        command = GetCommand(command);
        if (command != nullptr && !ParseCommand(command))
        {
            break;
        }
    }

    SetCommandExitStatus(CHIP_NO_ERROR);
    return CHIP_NO_ERROR;
}

bool InteractiveStartCommand::ParseCommand(char * command)
{
    if (strcmp(command, kInteractiveModeStopCommand) == 0)
    {
        ExecuteDeferredCleanups();
        return false;
    }

    char * args[kInteractiveModeArgumentsMaxLength];
    args[0]       = kInteractiveModeName;
    int argsCount = 1;

    char * token = strtok(command, " ");
    while (token != nullptr)
    {
        if (argsCount == kInteractiveModeArgumentsMaxLength)
        {
            gIsCommandRunning = true;
            ChipLogError(chipTool, "Too many arguments. Ignoring.");
            gIsCommandRunning = false;
            return true;
        }

        args[argsCount++] = token;
        token             = strtok(nullptr, " ");
    }

    ClearLine();
    gIsCommandRunning = true;
    mHandler->RunInteractive(argsCount, args);
    gIsCommandRunning = false;

    return true;
}
