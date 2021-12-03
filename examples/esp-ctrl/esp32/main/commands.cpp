/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <lib/shell/commands/Help.h>
#include <lib/shell/streamer.h>
#include <lib/support/BytesToHex.h>
#include <lib/support/CodeUtils.h>
#include <memory>
#include "commands.hpp"

/*BLE*/
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "blecent.hpp"

using chip::Shell::Engine;
using chip::Shell::PrintCommandHelp;
using chip::Shell::shell_command_t;
using chip::Shell::streamer_get;
using chip::Shell::streamer_printf;

namespace cli {
static int
blecent_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_hs_adv_fields fields;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
printf("Discovering devices nearby\n");
        rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                     event->disc.length_data);
        if (rc != 0) {
            return 0;
        }
        /* An advertisment report was received during GAP discovery. */
        print_adv_fields(&fields);
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        return 0;

    default:
        return 0;
    }
}

static void
blecent_scan(void)
{   
    uint8_t own_addr_type;
    struct ble_gap_disc_params disc_params;
    int rc;
    
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        printf("error determining address type; rc=%d\n", rc);
        return;
    }
    
    disc_params.filter_duplicates = 1;
    disc_params.passive = 1;
    disc_params.itvl = 0;
    disc_params.window = 0;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;
    
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params,
                      blecent_gap_event, NULL);
    if (rc != 0) {
printf("Disc failed\n");
    }
}

CHIP_ERROR ControlBleScanHandler(int argc, char **argv)
{ 
    printf("Starting ble-scan...\n");
    blecent_scan();
printf("Scanning complete\n");
    return CHIP_NO_ERROR;
}

Engine sControlSubCommands;

CHIP_ERROR ControlHelpHandler(int argc, char **argv)
{   
    sControlSubCommands.ForEachCommand(PrintCommandHelp, nullptr);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ControlDispatch(int argc, char **argv)
{
    if (argc == 0) {
        ControlHelpHandler(argc, argv);
        return CHIP_NO_ERROR;
    }

    return sControlSubCommands.ExecCommand(argc, argv);
}

void RegisterControlCommands()
{
    static const shell_command_t sHeapSubCommands[] = { 
        {&ControlHelpHandler, "help", "Usage: control <subcommand>"},
        {&ControlBleScanHandler, "blescan", "Scan all ble devices"},
        {&ControlPairingHandler, "blescan", "Scan all ble devices"},
    };  
    cli::sControlSubCommands.RegisterCommands(sHeapSubCommands, ArraySize(sHeapSubCommands));

    static const shell_command_t sHeapCommand = {&ControlDispatch, "control", "Control other nodes"};
    Engine::Root().RegisterCommands(&sHeapCommand, 1); 
}

} // namespace cli
