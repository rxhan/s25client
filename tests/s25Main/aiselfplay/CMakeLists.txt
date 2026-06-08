# Copyright (C) 2005 - 2026 Settlers Freaks <sf-team at siedler25.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Headless Self-Play-Test: AdvancedAI vs. AIPlayerJH (-> Target Test_aiselfplay)
add_testcase(NAME aiselfplay
    LIBS s25Main testHelpers testWorldFixtures testConfig
    COST 50
)
