async function startScan() {
    const btn = document.getElementById('main-scan-btn');
    const camText = document.getElementById('cam-text');
    const scanOverlay = document.getElementById('scan-overlay');
    const aiVerdict = document.getElementById('ai-verdict');
    const aiConf = document.getElementById('ai-conf');
    const tofDepth = document.getElementById('tof-depth');
    const statusInd = document.getElementById('system-status');
    
    // 1. Lock UI and show loading state
    btn.disabled = true;
    btn.textContent = 'COMMUNICATING WITH SENSORS...';
    camText.style.display = 'none';
    scanOverlay.style.display = 'block';
    
    aiVerdict.textContent = '--';
    aiVerdict.style.color = '#94a3b8';
    aiConf.textContent = 'Conf: --%';
    tofDepth.textContent = '-- mm';
    
    statusInd.className = 'status-indicator idle';
    statusInd.textContent = 'AWAITING HARDWARE RESPONSE...';
    
    try {
        // 2. Send the actual command to the ESP32-S3
        const response = await fetch('/run-diagnostic');
        
        // Check if the board crashed or disconnected
        if (!response.ok) {
            throw new Error('Hardware response failed');
        }
        
        // 3. Parse the real JSON data sent back from the C++ logic
        const data = await response.json();
        
        // 4. Update UI with the live physical data
        aiVerdict.textContent = data.ai_class; // e.g., "HEALTHY" or "DEFECTIVE"
        aiConf.textContent = `Conf: ${data.confidence}%`;
        tofDepth.textContent = `${data.tof_mm} mm`;
        
        statusInd.className = 'status-indicator';
        
        // 5. Apply the fused recommendation logic
        if (data.system_status === 'safe') {
            aiVerdict.style.color = '#22c55e';
            statusInd.textContent = 'SAFE TO DRIVE';
            statusInd.classList.add('safe');
        } else if (data.system_status === 'warning') {
            aiVerdict.style.color = '#22c55e';
            statusInd.textContent = 'WARNING: WORN';
            statusInd.classList.add('warning');
        } else if (data.system_status === 'critical') {
            aiVerdict.style.color = '#ef4444';
            statusInd.textContent = 'CRITICAL ALERT';
            statusInd.classList.add('critical');
        }

    } catch (error) {
        // 6. Handle hardware disconnections gracefully during the live demo
        console.error('Diagnostic error:', error);
        statusInd.className = 'status-indicator idle';
        statusInd.textContent = 'ERROR: HARDWARE TIMEOUT';
        statusInd.style.color = '#ef4444';
        statusInd.style.border = '2px dashed #ef4444';
    } finally {
        // 7. Reset the button so they can scan another tire
        btn.disabled = false;
        btn.textContent = 'START DIAGNOSTIC SCAN';
        camText.style.display = 'block';
        scanOverlay.style.display = 'none';
    }
}