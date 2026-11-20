const DeviceAPI = {
    async setGPIO(options) {
        // options: { port, pin, mode, pull, state }
        const params = new URLSearchParams({
            device: "gpio"
        });

        if (options.port !== undefined) params.append("port", options.port);
        if (options.pin !== undefined) params.append("pin", options.pin);
        if (options.mode !== undefined) params.append("mode", options.mode);
        if (options.pull !== undefined) params.append("pull", options.pull);
        if (options.state !== undefined) params.append("state", options.state);

        const response = await fetch(`/api?${params.toString()}`);

        if (!response.ok) {
            throw new Error(`HTTP error: ${response.status}`);
        }

        return true;
    }
};