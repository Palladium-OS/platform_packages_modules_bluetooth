/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.server.bluetooth;

import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import android.content.ContentResolver;
import android.content.Context;
import android.os.Looper;

import androidx.test.filters.SmallTest;
import androidx.test.runner.AndroidJUnit4;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.MockitoAnnotations;

@SmallTest
@RunWith(AndroidJUnit4.class)
public class BluetoothSatelliteModeListenerTest {

    private BluetoothSatelliteModeListener mBluetoothSatelliteModeListener;

    @Mock private Context mContext;
    @Mock private ContentResolver mContentResolver;
    @Mock private BluetoothManagerService mBluetoothManagerService;

    @Before
    public void setUp() throws Exception {
        MockitoAnnotations.initMocks(this);
        when(mContext.getContentResolver()).thenReturn(mContentResolver);

        mBluetoothSatelliteModeListener = new BluetoothSatelliteModeListener(
                mBluetoothManagerService, Looper.getMainLooper(), mContext);
    }

    @Test
    public void testHandleSatelliteModeChange_InvokeSatelliteModeChanged() {
        mBluetoothSatelliteModeListener.handleSatelliteModeChange();
        verify(mBluetoothManagerService).onSatelliteModeChanged();
    }
}

