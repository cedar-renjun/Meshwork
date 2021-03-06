package org.meshwork.core.host.l3;

import org.meshwork.core.AbstractMessage;
import org.meshwork.core.MessageData;

import java.io.IOException;
import java.io.PrintWriter;

/**
 * Created by Sinisha Djukic on 14-2-10.
 */
public class MInternal extends AbstractMessage implements Constants {

    public MInternal(byte seq) {
        super(seq, NS_CODE, NS_SUBCODE_INTERNAL);
    }

    @Override
    public void toString(PrintWriter writer, String rowPrefix, String rowSuffix, String separator) {
        writer.print("MInternal");
    }

    @Override
    public AbstractMessage deserialize(MessageData msg) throws IOException {
        MInternal result = new MInternal(msg.seq);
        return result;
    }

    @Override
    public void serializeImpl(MessageData msg) {
    }
}
