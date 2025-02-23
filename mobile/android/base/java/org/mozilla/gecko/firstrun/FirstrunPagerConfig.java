/* -*- Mode: Java; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.gecko.firstrun;

import android.content.Context;
import android.os.Bundle;
import android.util.Log;
import com.keepsafe.switchboard.SwitchBoard;
import org.mozilla.gecko.AppConstants;
import org.mozilla.gecko.R;
import org.mozilla.gecko.Telemetry;
import org.mozilla.gecko.TelemetryContract;

import java.util.LinkedList;
import java.util.List;

public class FirstrunPagerConfig {
    public static final String LOGTAG = "FirstrunPagerConfig";

    public static final String KEY_IMAGE = "imageRes";
    public static final String KEY_TEXT = "textRes";
    public static final String KEY_SUBTEXT = "subtextRes";

    public static final String ONBOARDING_A = "onboarding-a";
    public static final String ONBOARDING_B = "onboarding-b";
    public static final String ONBOARDING_C = "onboarding-c";

    public static List<FirstrunPanelConfig> getDefault(Context context) {
        final List<FirstrunPanelConfig> panels = new LinkedList<>();

        if (isInExperimentLocal(context, ONBOARDING_A)) {
            panels.add(new FirstrunPanelConfig(WelcomePanel.class.getName(), WelcomePanel.TITLE_RES));
            Telemetry.startUISession(TelemetryContract.Session.EXPERIMENT, ONBOARDING_A);
        } else if (isInExperimentLocal(context, ONBOARDING_B)) {
            panels.add(SimplePanelConfigs.urlbarPanelConfig);
            panels.add(SimplePanelConfigs.bookmarksPanelConfig);
            panels.add(SimplePanelConfigs.syncPanelConfig);
            panels.add(new FirstrunPanelConfig(SyncPanel.class.getName(), SyncPanel.TITLE_RES));
            Telemetry.startUISession(TelemetryContract.Session.EXPERIMENT, ONBOARDING_B);
        } else if (isInExperimentLocal(context, ONBOARDING_C)) {
            panels.add(SimplePanelConfigs.urlbarPanelConfig);
            panels.add(SimplePanelConfigs.bookmarksPanelConfig);
            panels.add(SimplePanelConfigs.dataPanelConfig);
            panels.add(SimplePanelConfigs.syncPanelConfig);
            panels.add(new FirstrunPanelConfig(SyncPanel.class.getName(), SyncPanel.TITLE_RES));
            Telemetry.startUISession(TelemetryContract.Session.EXPERIMENT, ONBOARDING_C);
        } else {
            Log.d(LOGTAG, "Not in an experiment!");
            panels.add(new FirstrunPanelConfig(WelcomePanel.class.getName(), WelcomePanel.TITLE_RES));
        }

        return panels;
    }

    /*
     * Wrapper method for using local bucketing rather than server-side.
     * This needs to match the server-side bucketing used on mozilla-switchboard.herokuapp.com.
     */
    private static boolean isInExperimentLocal(Context context, String name) {
        if (AppConstants.MOZ_SWITCHBOARD) {
            if (SwitchBoard.isInBucket(context, 0, 33)) {
                return ONBOARDING_A.equals(name);
            } else if (SwitchBoard.isInBucket(context, 33, 66)) {
                return ONBOARDING_B.equals(name);
            } else if (SwitchBoard.isInBucket(context, 66, 100)) {
                return ONBOARDING_C.equals(name);
            }
        }
        return false;
    }

    public static List<FirstrunPanelConfig> getRestricted() {
        final List<FirstrunPanelConfig> panels = new LinkedList<>();
        panels.add(new FirstrunPanelConfig(RestrictedWelcomePanel.class.getName(), RestrictedWelcomePanel.TITLE_RES));
        return panels;
    }

    public static class FirstrunPanelConfig {

        private String classname;
        private int titleRes;
        private Bundle args;

        public FirstrunPanelConfig(String resource, int titleRes) {
            this(resource, titleRes, -1, -1, -1, true);
        }

        public FirstrunPanelConfig(String classname, int titleRes, int imageRes, int textRes, int subtextRes) {
            this(classname, titleRes, imageRes, textRes, subtextRes, false);
        }

        private FirstrunPanelConfig(String classname, int titleRes, int imageRes, int textRes, int subtextRes, boolean isCustom) {
            this.classname = classname;
            this.titleRes = titleRes;

            if (!isCustom) {
                this.args = new Bundle();
                this.args.putInt(KEY_IMAGE, imageRes);
                this.args.putInt(KEY_TEXT, textRes);
                this.args.putInt(KEY_SUBTEXT, subtextRes);
            }
        }

        public String getClassname() {
            return this.classname;
        }

        public int getTitleRes() {
            return this.titleRes;
        }

        public Bundle getArgs() {
            return args;
        }
    }

    protected static class SimplePanelConfigs {
        public static final FirstrunPanelConfig urlbarPanelConfig = new FirstrunPanelConfig(FirstrunPanel.class.getName(), R.string.firstrun_panel_title_welcome, R.drawable.firstrun_urlbar, R.string.firstrun_urlbar_message, R.string.firstrun_urlbar_subtext);
        public static final FirstrunPanelConfig bookmarksPanelConfig = new FirstrunPanelConfig(FirstrunPanel.class.getName(), R.string.firstrun_bookmarks_title, R.drawable.firstrun_bookmarks, R.string.firstrun_bookmarks_message, R.string.firstrun_bookmarks_subtext);
        public static final FirstrunPanelConfig syncPanelConfig = new FirstrunPanelConfig(FirstrunPanel.class.getName(), R.string.firstrun_sync_title, R.drawable.firstrun_sync, R.string.firstrun_sync_message, R.string.firstrun_sync_subtext);
        public static final FirstrunPanelConfig dataPanelConfig = new FirstrunPanelConfig(DataPanel.class.getName(), R.string.firstrun_data_title, R.drawable.firstrun_data_off, R.string.firstrun_data_message, R.string.firstrun_data_subtext);
    }
}
