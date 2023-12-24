# Mati

Mati is a gstreamer based component that will pull in an rtsp stream and
attempt to do motion detection on it. It will also send out the stream again
over TCP so it can easily be watched in a browser. Whenever it detects motion,
it will start encoding the stream and writing to a file, so you can later view
what happened. Whenever someone requests to open a TCP client over dbus, it
will also start encoding and then send that stream to a TCP client. This way,
we can preserve resources if they aren't being used anyway.