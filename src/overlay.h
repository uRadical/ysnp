#ifndef YSNP_OVERLAY_H
#define YSNP_OVERLAY_H

/* image_path is a file path, or NULL to use the embedded default */
void overlay_show(const char *image_path);
void overlay_run(void);   /* enters event loop, blocks until dismissed */
void overlay_close(void); /* called from input handlers to exit the loop */

#endif /* YSNP_OVERLAY_H */
