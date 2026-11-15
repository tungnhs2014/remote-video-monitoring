#include <gst/gst.h>

int main(int argc, char* argv[])
{
    GError* error = NULL;
    if (!gst_init_check(&argc, &argv, &error))
    {
        g_clear_error(&error);
        return 1;
    }

    GstElement* pipeline = gst_pipeline_new("m1-dependency-smoke");
    if (pipeline == NULL)
    {
        return 1;
    }

    gst_object_unref(pipeline);
    return 0;
}
